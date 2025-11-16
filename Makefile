################################################################################
# Makefile para Sistema de Vigilancia ROI
# Universidad de Costa Rica - IE0301
################################################################################

CXX := g++
TARGET := secure_roi

# Detectar arquitectura
ARCH := $(shell uname -m)

# Directorios de CUDA (opcional)
CUDA_PATH ?= /usr/local/cuda-10.2
CUDA_INC := $(CUDA_PATH)/targets/aarch64-linux/include
CUDA_LIB := $(CUDA_PATH)/targets/aarch64-linux/lib

# Verificar si CUDA existe
CUDA_EXISTS := $(shell test -d $(CUDA_PATH) && echo yes || echo no)

ifeq ($(CUDA_EXISTS),yes)
    CUDA_CFLAGS := -I$(CUDA_INC)
    CUDA_LDFLAGS := -L$(CUDA_LIB) -Wl,-rpath,$(CUDA_LIB)
    CUDA_LIBS := -lcudart
else
    CUDA_CFLAGS :=
    CUDA_LDFLAGS :=
    CUDA_LIBS :=
    $(warning CUDA not found - compiling without CUDA support)
endif

# Directorios de DeepStream - Ajustar según tu instalación
# Opciones comunes:
# /opt/nvidia/deepstream/deepstream
# /opt/nvidia/deepstream/deepstream-6.0
# /usr/local/deepstream
DS_PATH := /opt/nvidia/deepstream/deepstream-6.0
DS_INC := $(DS_PATH)/sources/includes
DS_LIB := $(DS_PATH)/lib

# Obtener flags de pkg-config
GST_CFLAGS := $(shell pkg-config --cflags gstreamer-1.0 gstreamer-base-1.0 glib-2.0)
GST_LIBS := $(shell pkg-config --libs gstreamer-1.0 gstreamer-base-1.0 glib-2.0)

# Flags de compilación
CXXFLAGS := -std=c++11 -Wall \
            -I$(DS_INC) \
            $(CUDA_CFLAGS) \
            $(GST_CFLAGS)

# Flags de linkeo
LDFLAGS := -L$(DS_LIB) \
           $(CUDA_LDFLAGS) \
           -Wl,-rpath,$(DS_LIB)

LIBS := $(GST_LIBS) \
        -lnvdsgst_meta \
        -lnvds_meta \
        -lnvdsgst_helper \
        -lnvbufsurface \
        -lpthread \
        $(CUDA_LIBS)

# Archivos fuente
SRCS := secure_roi.cpp
OBJS := $(SRCS:.cpp=.o)

# Regla principal
.PHONY: all clean check-deps install-deps help

all: check-deps $(TARGET)

# Verificar dependencias
check-deps:
	@echo "Verificando dependencias..."
	@command -v pkg-config >/dev/null 2>&1 || { echo "ERROR: pkg-config no está instalado"; exit 1; }
	@pkg-config --exists gstreamer-1.0 || { echo "ERROR: gstreamer-1.0 no encontrado. Ejecute 'make install-deps'"; exit 1; }
	@pkg-config --exists glib-2.0 || { echo "ERROR: glib-2.0 no encontrado. Ejecute 'make install-deps'"; exit 1; }
	@test -d $(DS_PATH) || { echo "ERROR: DeepStream no encontrado en $(DS_PATH)"; exit 1; }
	@test -f secure_roi.cpp || { echo "ERROR: No se encuentra secure_roi.cpp"; exit 1; }
	@echo "✓ Todas las dependencias están disponibles"

# Compilar ejecutable
$(TARGET): $(OBJS)
	@echo "Linking $(TARGET)..."
	$(CXX) $(OBJS) $(LDFLAGS) $(LIBS) -o $(TARGET)
	@echo "✓ Build complete: $(TARGET)"
	@echo ""
	@echo "Para ejecutar:"
	@echo "  ./$(TARGET) vi-file <video.mp4> [opciones]"

# Compilar objetos
%.o: %.cpp
	@echo "Compiling $<..."
	@echo "CXXFLAGS: $(CXXFLAGS)"
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Limpiar archivos generados
clean:
	@echo "Cleaning..."
	rm -f $(OBJS) $(TARGET)
	rm -f *.txt *.mp4
	@echo "✓ Clean complete"

# Instalar dependencias
install-deps:
	@echo "Instalando dependencias..."
	sudo apt-get update
	sudo apt-get install -y \
		build-essential \
		pkg-config \
		libgstreamer1.0-dev \
		libgstreamer-plugins-base1.0-dev \
		libglib2.0-dev \
		gstreamer1.0-tools \
		gstreamer1.0-plugins-base \
		gstreamer1.0-plugins-good
	@echo "✓ Dependencias instaladas"

# Mostrar información del sistema
info:
	@echo "=== Información del Sistema ==="
	@echo "Arquitectura: $(ARCH)"
	@echo "CUDA path: $(CUDA_PATH)"
	@echo "CUDA include: $(CUDA_INC)"
	@echo "CUDA lib: $(CUDA_LIB)"
	@echo "DeepStream path: $(DS_PATH)"
	@echo "GStreamer CFLAGS: $(GST_CFLAGS)"
	@echo "GStreamer LIBS: $(GST_LIBS)"
	@echo ""
	@echo "=== Verificación de componentes ==="
	@pkg-config --modversion gstreamer-1.0 2>/dev/null && echo "✓ GStreamer instalado" || echo "✗ GStreamer NO instalado"
	@test -d $(DS_PATH) && echo "✓ DeepStream encontrado" || echo "✗ DeepStream NO encontrado"
	@test -d $(CUDA_PATH) && echo "✓ CUDA encontrado" || echo "✗ CUDA NO encontrado"
	@test -f $(CUDA_INC)/cuda_runtime_api.h && echo "✓ CUDA headers encontrados" || echo "✗ CUDA headers NO encontrados"
	@test -f secure_roi.cpp && echo "✓ secure_roi.cpp encontrado" || echo "✗ secure_roi.cpp NO encontrado"

# Ejecutar con video de ejemplo
run-example:
	@test -f $(TARGET) || { echo "ERROR: Primero compile con 'make'"; exit 1; }
	./$(TARGET) vi-file /opt/nvidia/deepstream/deepstream/samples/streams/sample_720p.h264 \
		--left 0.3 --top 0.3 --width 0.4 --height 0.4 \
		--time 5 --file-name report.txt \
		vo-file output.mp4 --mode video

# Ayuda
help:
	@echo "Makefile para Sistema de Vigilancia ROI"
	@echo ""
	@echo "Targets disponibles:"
	@echo "  make              - Compila el proyecto"
	@echo "  make clean        - Limpia archivos generados"
	@echo "  make install-deps - Instala dependencias necesarias"
	@echo "  make info         - Muestra información del sistema"
	@echo "  make check-deps   - Verifica dependencias"
	@echo "  make run-example  - Ejecuta con video de ejemplo"
	@echo "  make help         - Muestra esta ayuda"
	@echo ""
	@echo "Uso manual:"
	@echo "  ./secure_roi vi-file <input.mp4> [opciones]"

.PHONY: all clean install-deps info check-deps run-example help
