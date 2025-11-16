#!/bin/bash
################################################################################
# Script de Pruebas para Sistema de Vigilancia ROI
# Universidad de Costa Rica - IE0301
################################################################################

# Colores para output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=== Sistema de Pruebas - Vigilancia ROI ==="
echo ""

# Verificar que el ejecutable existe
if [ ! -f "./secure_roi" ]; then
    echo -e "${RED}Error: No se encuentra el ejecutable 'secure_roi'${NC}"
    echo "Ejecute 'make' primero para compilar el proyecto"
    exit 1
fi

# Directorio para resultados
RESULTS_DIR="test_results"
mkdir -p "$RESULTS_DIR"

# Video de entrada (ajustar según disponibilidad)
INPUT_VIDEO="/opt/nvidia/deepstream/deepstream/samples/streams/sample_1080p_h264.mp4"

# Verificar que existe el video
if [ ! -f "$INPUT_VIDEO" ]; then
    echo -e "${YELLOW}Advertencia: Video de ejemplo no encontrado${NC}"
    echo "Por favor, especifique la ruta a un video válido"
    read -p "Ruta del video de entrada: " INPUT_VIDEO
    
    if [ ! -f "$INPUT_VIDEO" ]; then
        echo -e "${RED}Error: El archivo no existe${NC}"
        exit 1
    fi
fi

echo -e "${GREEN}Video de entrada: $INPUT_VIDEO${NC}"
echo ""

################################################################################
# ESCENARIO 1: Objeto dentro del ROI sin superar tiempo máximo
################################################################################
echo -e "${YELLOW}=== ESCENARIO 1: Objeto NO supera tiempo máximo ===${NC}"
echo "ROI: Centro de la imagen, Tiempo máximo: 10 segundos"

./secure_roi vi-file "$INPUT_VIDEO" \
    --left 0.35 --top 0.35 --width 0.3 --height 0.3 \
    --time 10 \
    --file-name "$RESULTS_DIR/report_scenario1.txt" \
    vo-file "$RESULTS_DIR/output_scenario1.mp4" \
    --mode video

echo -e "${GREEN}Escenario 1 completado${NC}"
echo "Salida: $RESULTS_DIR/output_scenario1.mp4"
echo "Reporte: $RESULTS_DIR/report_scenario1.txt"
echo ""

################################################################################
# ESCENARIO 2: Objeto dentro del ROI superando tiempo máximo
################################################################################
echo -e "${YELLOW}=== ESCENARIO 2: Objeto SUPERA tiempo máximo ===${NC}"
echo "ROI: Centro de la imagen, Tiempo máximo: 2 segundos"

./secure_roi vi-file "$INPUT_VIDEO" \
    --left 0.35 --top 0.35 --width 0.3 --height 0.3 \
    --time 2 \
    --file-name "$RESULTS_DIR/report_scenario2.txt" \
    vo-file "$RESULTS_DIR/output_scenario2.mp4" \
    --mode video

echo -e "${GREEN}Escenario 2 completado${NC}"
echo "Salida: $RESULTS_DIR/output_scenario2.mp4"
echo "Reporte: $RESULTS_DIR/report_scenario2.txt"
echo ""

################################################################################
# ESCENARIO 3: ROI grande para capturar múltiples objetos
################################################################################
echo -e "${YELLOW}=== ESCENARIO 3: Múltiples objetos en ROI ===${NC}"
echo "ROI: Grande (70% de la imagen), Tiempo máximo: 3 segundos"

./secure_roi vi-file "$INPUT_VIDEO" \
    --left 0.15 --top 0.15 --width 0.7 --height 0.7 \
    --time 3 \
    --file-name "$RESULTS_DIR/report_scenario3.txt" \
    vo-file "$RESULTS_DIR/output_scenario3.mp4" \
    --mode video

echo -e "${GREEN}Escenario 3 completado${NC}"
echo "Salida: $RESULTS_DIR/output_scenario3.mp4"
echo "Reporte: $RESULTS_DIR/report_scenario3.txt"
echo ""

################################################################################
# ESCENARIO 4: Streaming por UDP
################################################################################
echo -e "${YELLOW}=== ESCENARIO 4: Streaming UDP ===${NC}"
echo "Configuración UDP - Host: 127.0.0.1, Puerto: 5000"
echo -e "${YELLOW}NOTA: Para ver el stream, ejecute en otra terminal:${NC}"
echo "gst-launch-1.0 udpsrc port=5000 ! application/x-rtp,encoding-name=H264,payload=96 ! rtph264depay ! h264parse ! avdec_h264 ! autovideosink"
echo ""
read -p "Presione Enter para iniciar streaming (Ctrl+C para detener)..."

./secure_roi vi-file "$INPUT_VIDEO" \
    --left 0.4 --top 0.4 --width 0.2 --height 0.2 \
    --time 5 \
    --file-name "$RESULTS_DIR/report_scenario4.txt" \
    --mode udp \
    --udp-host 127.0.0.1 \
    --udp-port 5000

echo -e "${GREEN}Escenario 4 completado${NC}"
echo ""

################################################################################
# RESUMEN DE RESULTADOS
################################################################################
echo -e "${GREEN}=== RESUMEN DE PRUEBAS ===${NC}"
echo ""
echo "Todos los archivos generados están en: $RESULTS_DIR/"
echo ""
echo "Videos de salida:"
ls -lh "$RESULTS_DIR"/*.mp4 2>/dev/null || echo "  (ninguno)"
echo ""
echo "Reportes generados:"
ls -lh "$RESULTS_DIR"/*.txt 2>/dev/null || echo "  (ninguno)"
echo ""

# Mostrar contenido de los reportes
for report in "$RESULTS_DIR"/report_*.txt; do
    if [ -f "$report" ]; then
        echo -e "${YELLOW}--- Contenido de $(basename $report) ---${NC}"
        cat "$report"
        echo ""
    fi
done

echo -e "${GREEN}Todas las pruebas completadas exitosamente!${NC}"
echo ""
echo "Para reproducir los videos:"
echo "  gst-play-1.0 $RESULTS_DIR/output_scenario1.mp4"
echo "  # o usar cualquier reproductor de video"
