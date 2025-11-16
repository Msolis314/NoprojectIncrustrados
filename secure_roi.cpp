/*
 * Sistema de Vigilancia con ROI para Jetson Nano
 * Detecta vehículos/personas y monitorea tiempo en ROI
 * Universidad de Costa Rica - IE0301
 */

#include <gst/gst.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
//#include <cuda_runtime_api.h>
#include <sys/time.h>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <iomanip>
#include "gstnvdsmeta.h"

// Parámetros de la aplicación
struct AppConfig {
    gchar *input_file;
    gchar *output_file;
    gchar *report_file;
    gfloat roi_left;
    gfloat roi_top;
    gfloat roi_width;
    gfloat roi_height;
    gint max_time_seconds;
    gchar *mode;
    gint udp_port;
    gchar *udp_host;
};

// ROI normalizado (0-1)
struct ROIParams {
    float x, y, w, h;
};

// Estados del objeto
enum ObjectState {
    STATE_OUTSIDE,
    STATE_INSIDE,
    STATE_ALERT
};

// Información de seguimiento por objeto
struct TrackInfo {
    guint64 track_id;
    ObjectState state;
    GTimer *timer;
    gdouble total_time;
    gdouble entry_timestamp;
    gchar *class_name;
    gboolean alert_triggered;
};

// Contexto global
struct AppContext {
    GstElement *pipeline;
    GMainLoop *loop;
    AppConfig config;
    ROIParams roi;
    std::unordered_map<guint64, TrackInfo> tracked_objects;
    GTimer *app_timer;
    guint total_detected;
    guint total_alerts;
    gint source_width;
    gint source_height;
    gboolean roi_has_objects;
    gboolean roi_has_alerts;
};

static AppContext g_app_ctx;

// Utilidad para color
static inline void set_color(NvOSD_ColorParams &c, float r, float g, float b, float a = 1.0f) {
    c.red = r; c.green = g; c.blue = b; c.alpha = a;
}

// Verifica si el centro del bbox está dentro del ROI
static gboolean is_bbox_in_roi(NvOSD_RectParams *bbox, const ROIParams &roi, 
                                gint frame_width, gint frame_height) {
    float cx = (bbox->left + bbox->width / 2.0f) / frame_width;
    float cy = (bbox->top + bbox->height / 2.0f) / frame_height;
    return (cx >= roi.x && cx <= (roi.x + roi.w) && cy >= roi.y && cy <= (roi.y + roi.h));
}

// Dibuja el ROI
static void draw_roi_rect(NvDsBatchMeta *batch_meta, NvDsFrameMeta *fmeta,
                          const ROIParams &roi, gboolean any_inside, gboolean any_alert) {
    NvDsDisplayMeta *disp_meta = nvds_acquire_display_meta_from_pool(batch_meta);
    disp_meta->num_rects = 1;
    
    NvOSD_RectParams &r = disp_meta->rect_params[0];
    r.left = (gint)(roi.x * fmeta->source_frame_width);
    r.top = (gint)(roi.y * fmeta->source_frame_height);
    r.width = (gint)(roi.w * fmeta->source_frame_width);
    r.height = (gint)(roi.h * fmeta->source_frame_height);
    r.border_width = 4;
    
    if (any_alert) {
        r.has_bg_color = 1;
        set_color(r.bg_color, 1.0f, 0.0f, 0.0f, 0.3f);
        set_color(r.border_color, 1.0f, 0.0f, 0.0f);
    } else if (any_inside) {
        r.has_bg_color = 1;
        set_color(r.bg_color, 1.0f, 0.65f, 0.0f, 0.3f);
        set_color(r.border_color, 1.0f, 0.65f, 0.0f);
    } else {
        r.has_bg_color = 0;
        set_color(r.border_color, 0.0f, 1.0f, 0.0f);
    }
    
    nvds_add_display_meta_to_frame(fmeta, disp_meta);
}

// Pad probe para procesar objetos
static GstPadProbeReturn osd_sink_pad_buffer_probe(GstPad *pad, GstPadProbeInfo *info, gpointer u_data) {
    GstBuffer *buf = (GstBuffer *)info->data;
    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta(buf);
    
    if (!batch_meta) return GST_PAD_PROBE_OK;
    
    g_app_ctx.roi_has_objects = FALSE;
    g_app_ctx.roi_has_alerts = FALSE;
    std::unordered_map<guint64, bool> active_tracks;
    
    for (NvDsMetaList *l_frame = batch_meta->frame_meta_list; l_frame; l_frame = l_frame->next) {
        NvDsFrameMeta *fmeta = (NvDsFrameMeta *)l_frame->data;
        
        if (g_app_ctx.source_width == 0) {
            g_app_ctx.source_width = fmeta->source_frame_width;
            g_app_ctx.source_height = fmeta->source_frame_height;
        }
        
        for (NvDsMetaList *l_obj = fmeta->obj_meta_list; l_obj; l_obj = l_obj->next) {
            NvDsObjectMeta *obj_meta = (NvDsObjectMeta *)l_obj->data;
            if (!obj_meta) continue;
            if (obj_meta->class_id != 0 && obj_meta->class_id != 2) continue;
            
            guint64 track_id = obj_meta->object_id;
            active_tracks[track_id] = true;
            
            gboolean inside_roi = is_bbox_in_roi(&obj_meta->rect_params, g_app_ctx.roi,
                                                  fmeta->source_frame_width, fmeta->source_frame_height);
            
            TrackInfo *track_info;
            auto it = g_app_ctx.tracked_objects.find(track_id);
            
            if (it == g_app_ctx.tracked_objects.end()) {
                TrackInfo new_track;
                new_track.track_id = track_id;
                new_track.state = STATE_OUTSIDE;
                new_track.timer = g_timer_new();
                new_track.total_time = 0.0;
                new_track.entry_timestamp = 0.0;
                new_track.class_name = g_strdup(obj_meta->obj_label);
                new_track.alert_triggered = FALSE;
                g_app_ctx.tracked_objects[track_id] = new_track;
                track_info = &g_app_ctx.tracked_objects[track_id];
                g_app_ctx.total_detected++;
            } else {
                track_info = &it->second;
            }
            
            if (inside_roi) {
                g_app_ctx.roi_has_objects = TRUE;
                
                if (track_info->state == STATE_OUTSIDE) {
                    track_info->state = STATE_INSIDE;
                    g_timer_start(track_info->timer);
                    track_info->entry_timestamp = g_timer_elapsed(g_app_ctx.app_timer, NULL);
                } else if (track_info->state == STATE_INSIDE) {
                    gdouble elapsed = g_timer_elapsed(track_info->timer, NULL);
                    if (elapsed >= g_app_ctx.config.max_time_seconds) {
                        track_info->state = STATE_ALERT;
                        track_info->alert_triggered = TRUE;
                        g_app_ctx.total_alerts++;
                    }
                }
                
                if (track_info->state == STATE_ALERT) {
                    g_app_ctx.roi_has_alerts = TRUE;
                    set_color(obj_meta->rect_params.border_color, 1.0f, 0.0f, 0.0f);
                    obj_meta->rect_params.border_width = 4;
                } else {
                    set_color(obj_meta->rect_params.border_color, 1.0f, 0.65f, 0.0f);
                    obj_meta->rect_params.border_width = 3;
                }
            } else {
                if (track_info->state != STATE_OUTSIDE) {
                    track_info->total_time = g_timer_elapsed(track_info->timer, NULL);
                    g_timer_stop(track_info->timer);
                    track_info->state = STATE_OUTSIDE;
                }
                set_color(obj_meta->rect_params.border_color, 0.0f, 1.0f, 0.0f);
                obj_meta->rect_params.border_width = 2;
            }
        }
        
        draw_roi_rect(batch_meta, fmeta, g_app_ctx.roi, g_app_ctx.roi_has_objects, g_app_ctx.roi_has_alerts);
    }
    
    return GST_PAD_PROBE_OK;
}

// Genera el reporte final
static void generate_report() {
    std::ofstream report(g_app_ctx.config.report_file);
    if (!report.is_open()) {
        g_printerr("Error: No se pudo crear el reporte\n");
        return;
    }
    
    gint roi_left = (gint)(g_app_ctx.roi.x * g_app_ctx.source_width);
    gint roi_top = (gint)(g_app_ctx.roi.y * g_app_ctx.source_height);
    gint roi_width = (gint)(g_app_ctx.roi.w * g_app_ctx.source_width);
    gint roi_height = (gint)(g_app_ctx.roi.h * g_app_ctx.source_height);
    
    report << "ROI: left: " << roi_left << " top: " << roi_top << " width: " << roi_width << " height: " << roi_height << "\n";
    report << "Max time: " << g_app_ctx.config.max_time_seconds << "s\n";
    report << "Detected: " << g_app_ctx.total_detected << " (" << g_app_ctx.total_alerts << ")\n";
    
    for (const auto &pair : g_app_ctx.tracked_objects) {
        const TrackInfo &info = pair.second;
        gdouble time_in_roi = (info.state != STATE_OUTSIDE) ? g_timer_elapsed(info.timer, NULL) : info.total_time;
        
        if (time_in_roi > 0.1) {
            gint minutes = (gint)(info.entry_timestamp / 60);
            gint seconds = (gint)(info.entry_timestamp) % 60;
            report << minutes << ":" << std::setfill('0') << std::setw(2) << seconds 
                   << " " << (info.class_name ? info.class_name : "object")
                   << " time " << (gint)time_in_roi << "s";
            if (info.alert_triggered) report << " alert";
            report << "\n";
        }
    }
    
    report.close();
    g_print("Reporte generado: %s\n", g_app_ctx.config.report_file);
}

// Bus callback
static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data) {
    GMainLoop *loop = (GMainLoop *)data;
    
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            g_print("End of stream\n");
            generate_report();
            g_main_loop_quit(loop);
            break;
        case GST_MESSAGE_ERROR: {
            gchar *debug;
            GError *error;
            gst_message_parse_error(msg, &error, &debug);
            g_printerr("ERROR: %s\n", error->message);
            g_free(debug);
            g_error_free(error);
            g_main_loop_quit(loop);
            break;
        }
        default:
            break;
    }
    return TRUE;
}

// Callback para pad dinámico
static void on_pad_added(GstElement *element, GstPad *pad, gpointer data) {
    GstElement *parser = GST_ELEMENT(data);
    GstPad *sinkpad = gst_element_get_static_pad(parser, "sink");
    if (!sinkpad) {
        g_printerr("on_pad_added: could not get parser sink pad\n");
        return;
    }

    if (gst_pad_is_linked(sinkpad)) {
        gst_object_unref(sinkpad);
        return;
    }

    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps) {
        caps = gst_pad_query_caps(pad, NULL);
    }
    if (!caps) {
        g_printerr("on_pad_added: could not get caps from demux pad\n");
        gst_object_unref(sinkpad);
        return;
    }

    const GstStructure *str = gst_caps_get_structure(caps, 0);
    const gchar *name = gst_structure_get_name(str);

    if (g_str_has_prefix(name, "video")) {
        if (GST_PAD_LINK_FAILED(gst_pad_link(pad, sinkpad))) {
            g_printerr("Error: Failed to link demuxer to parser\n");
        } else {
            g_print("Linked demuxer pad (%s) to parser\n", name);
        }
    }

    gst_caps_unref(caps);
    gst_object_unref(sinkpad);
}

// Crea el pipeline
static gboolean create_pipeline() {
    GstElement *source, *demux, *parser, *decoder, *streammux;
    GstElement *pgie, *tracker, *nvvidconv, *nvosd;
    GstElement *nvvidconv2, *capsfilter, *encoder, *parser2, *mux, *sink;
    GstPad *osd_sink_pad;
    GstBus *bus;

    g_app_ctx.pipeline = gst_pipeline_new("secure-roi-pipeline");
    if (!g_app_ctx.pipeline) {
        g_printerr("Failed to create pipeline\n");
        return FALSE;
    }

    /* Source & decode */
    source     = gst_element_factory_make("filesrc",        "file-source");
    demux      = gst_element_factory_make("qtdemux",        "demuxer");
    parser     = gst_element_factory_make("h264parse",      "h264-parser");
    decoder    = gst_element_factory_make("nvv4l2decoder",  "decoder");

    /* DeepStream core */
    streammux  = gst_element_factory_make("nvstreammux",    "stream-muxer");
    pgie       = gst_element_factory_make("nvinfer",        "primary-infer");
    tracker    = gst_element_factory_make("nvtracker",      "tracker");
    nvvidconv  = gst_element_factory_make("nvvideoconvert", "nvvideo-converter");
    nvosd      = gst_element_factory_make("nvdsosd",        "nv-onscreendisplay");

    /* Post-OSD -> encoder -> mp4 */
    nvvidconv2 = gst_element_factory_make("nvvideoconvert", "post-osd-conv");
    capsfilter = gst_element_factory_make("capsfilter",     "capsfilter");
    encoder    = gst_element_factory_make("nvv4l2h264enc",  "h264-encoder");
    parser2    = gst_element_factory_make("h264parse",      "h264-parser-out");
    mux        = gst_element_factory_make("qtmux",          "mp4-muxer");
    sink       = gst_element_factory_make("filesink",       "file-sink");

    if (!source || !demux || !parser || !decoder || !streammux ||
        !pgie || !tracker || !nvvidconv || !nvosd ||
        !nvvidconv2 || !capsfilter || !encoder || !parser2 || !mux || !sink) {
        g_printerr("Failed to create one or more elements\n");
        return FALSE;
    }

    /* Configure elements */
    g_object_set(G_OBJECT(source), "location", g_app_ctx.config.input_file, NULL);

    g_object_set(G_OBJECT(streammux),
                 "batch-size", 1,
                 "width", 1920,
                 "height", 1080,
                 "batched-push-timeout", 4000000, /* usec */
                 NULL);

    g_object_set(G_OBJECT(pgie),
                 "config-file-path",
                 "/opt/nvidia/deepstream/deepstream/samples/configs/deepstream-app/config_infer_primary.txt",
                 NULL);

    g_object_set(G_OBJECT(tracker),
                 "tracker-width", 640,
                 "tracker-height", 384,
                 "ll-lib-file", "/opt/nvidia/deepstream/deepstream/lib/libnvds_nvmultiobjecttracker.so",
                 "ll-config-file",
                 "/opt/nvidia/deepstream/deepstream/samples/configs/deepstream-app/config_tracker_NvDCF_perf.yml",
                 NULL);

    /* Encoder settings (tune as you like) */
    g_object_set(G_OBJECT(encoder),
                 "bitrate", 4000000,         /* 4 Mbps */
                 "preset-level", 1,
                 "insert-sps-pps", TRUE,
                 "iframeinterval", 30,
                 NULL);

    /* Filesink uses the output_file passed via CLI (vo-file) */
    g_object_set(G_OBJECT(sink),
                 "location", g_app_ctx.config.output_file,
                 "sync", FALSE,          /* don't sync to clock (offline encode) */
                 "async", FALSE,
                 NULL);

    /* Caps: ensure NVMM + NV12 for the encoder */
    GstCaps *caps = gst_caps_from_string("video/x-raw(memory:NVMM), format=NV12");
    g_object_set(G_OBJECT(capsfilter), "caps", caps, NULL);
    gst_caps_unref(caps);

    /* Add all elements to the bin */
    gst_bin_add_many(GST_BIN(g_app_ctx.pipeline),
                     source, demux, parser, decoder,
                     streammux, pgie, tracker,
                     nvvidconv, nvosd,
                     nvvidconv2, capsfilter, encoder, parser2, mux, sink,
                     NULL);

    /* filesrc -> demux */
    if (!gst_element_link(source, demux)) {
        g_printerr("Failed to link source -> demux\n");
        return FALSE;
    }

    /* Connect dynamic pad from demux to parser */
    g_signal_connect(demux, "pad-added", G_CALLBACK(on_pad_added), parser);

    /* parser -> decoder */
    if (!gst_element_link_many(parser, decoder, NULL)) {
        g_printerr("Failed to link parser -> decoder\n");
        return FALSE;
    }

    /* decoder -> streammux (request pad) */
    GstPad *decoder_src = gst_element_get_static_pad(decoder, "src");
    GstPad *mux_sink = gst_element_get_request_pad(streammux, "sink_0");
    if (gst_pad_link(decoder_src, mux_sink) != GST_PAD_LINK_OK) {
        g_printerr("Failed to link decoder -> streammux\n");
        gst_object_unref(decoder_src);
        gst_object_unref(mux_sink);
        return FALSE;
    }
    gst_object_unref(decoder_src);
    gst_object_unref(mux_sink);

    /* DeepStream path + encoder branch:
       streammux -> pgie -> tracker -> nvvidconv -> nvosd
       -> nvvidconv2 -> capsfilter -> encoder -> parser2 -> mux -> sink
    */
    if (!gst_element_link_many(streammux, pgie, tracker,
                               nvvidconv, nvosd,
                               nvvidconv2, capsfilter,
                               encoder, parser2, mux, sink, NULL)) {
        g_printerr("Failed to link main pipeline (mux->sink)\n");
        return FALSE;
    }

    /* OSD pad probe for ROI logic */
    osd_sink_pad = gst_element_get_static_pad(nvosd, "sink");
    if (!osd_sink_pad) {
        g_printerr("Failed to get osd sink pad\n");
        return FALSE;
    }
    gst_pad_add_probe(osd_sink_pad, GST_PAD_PROBE_TYPE_BUFFER,
                      osd_sink_pad_buffer_probe, NULL, NULL);
    gst_object_unref(osd_sink_pad);

    /* Bus */
    bus = gst_pipeline_get_bus(GST_PIPELINE(g_app_ctx.pipeline));
    gst_bus_add_watch(bus, bus_call, g_app_ctx.loop);
    gst_object_unref(bus);

    return TRUE;
}


// Cleanup
static void cleanup() {
    for (auto &pair : g_app_ctx.tracked_objects) {
        if (pair.second.timer) g_timer_destroy(pair.second.timer);
        if (pair.second.class_name) g_free(pair.second.class_name);
    }
    g_app_ctx.tracked_objects.clear();
    
    if (g_app_ctx.app_timer) g_timer_destroy(g_app_ctx.app_timer);
    if (g_app_ctx.pipeline) {
        gst_element_set_state(g_app_ctx.pipeline, GST_STATE_NULL);
        gst_object_unref(GST_OBJECT(g_app_ctx.pipeline));
    }
    
    g_free(g_app_ctx.config.input_file);
    g_free(g_app_ctx.config.output_file);
    g_free(g_app_ctx.config.report_file);
    g_free(g_app_ctx.config.mode);
    g_free(g_app_ctx.config.udp_host);
}

// Parse argumentos
static gboolean parse_arguments(int argc, char *argv[]) {
    g_app_ctx.config.roi_left = 0.3f;
    g_app_ctx.config.roi_top = 0.3f;
    g_app_ctx.config.roi_width = 0.4f;
    g_app_ctx.config.roi_height = 0.4f;
    g_app_ctx.config.max_time_seconds = 5;
    g_app_ctx.config.report_file = g_strdup("report.txt");
    g_app_ctx.config.output_file = g_strdup("output.mp4");
    g_app_ctx.config.mode = g_strdup("video");
    g_app_ctx.config.udp_port = 5000;
    g_app_ctx.config.udp_host = g_strdup("127.0.0.1");
    g_app_ctx.config.input_file = NULL;
    
    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--left") == 0 && i + 1 < argc) {
            g_app_ctx.config.roi_left = g_strtod(argv[++i], NULL);
        } else if (g_strcmp0(argv[i], "--top") == 0 && i + 1 < argc) {
            g_app_ctx.config.roi_top = g_strtod(argv[++i], NULL);
        } else if (g_strcmp0(argv[i], "--width") == 0 && i + 1 < argc) {
            g_app_ctx.config.roi_width = g_strtod(argv[++i], NULL);
        } else if (g_strcmp0(argv[i], "--height") == 0 && i + 1 < argc) {
            g_app_ctx.config.roi_height = g_strtod(argv[++i], NULL);
        } else if (g_strcmp0(argv[i], "--time") == 0 && i + 1 < argc) {
            g_app_ctx.config.max_time_seconds = atoi(argv[++i]);
        } else if (g_strcmp0(argv[i], "--file-name") == 0 && i + 1 < argc) {
            g_free(g_app_ctx.config.report_file);
            g_app_ctx.config.report_file = g_strdup(argv[++i]);
        } else if (g_strcmp0(argv[i], "vi-file") == 0 && i + 1 < argc) {
            g_app_ctx.config.input_file = g_strdup(argv[++i]);
        } else if (g_strcmp0(argv[i], "vo-file") == 0 && i + 1 < argc) {
            g_free(g_app_ctx.config.output_file);
            g_app_ctx.config.output_file = g_strdup(argv[++i]);
        } else if (g_strcmp0(argv[i], "--mode") == 0 && i + 1 < argc) {
            g_free(g_app_ctx.config.mode);
            g_app_ctx.config.mode = g_strdup(argv[++i]);
        }
    }
    
    if (!g_app_ctx.config.input_file) {
        g_printerr("Error: Debe especificar vi-file <input>\n");
        g_printerr("Uso: %s vi-file <input.mp4> [opciones]\n", argv[0]);
        return FALSE;
    }
    
    g_app_ctx.roi.x = g_app_ctx.config.roi_left;
    g_app_ctx.roi.y = g_app_ctx.config.roi_top;
    g_app_ctx.roi.w = g_app_ctx.config.roi_width;
    g_app_ctx.roi.h = g_app_ctx.config.roi_height;
    
    return TRUE;
}

// Main
int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);
   // memset(&g_app_ctx, 0, sizeof(AppContext));
    g_app_ctx.app_timer = g_timer_new();
    
    if (!parse_arguments(argc, argv)) {
        cleanup();
        return -1;
    }
    
    g_print("=== Sistema de Vigilancia ROI ===\n");
    g_print("Input: %s\n", g_app_ctx.config.input_file);
    g_print("ROI: [%.2f, %.2f, %.2f, %.2f]\n", g_app_ctx.roi.x, g_app_ctx.roi.y, g_app_ctx.roi.w, g_app_ctx.roi.h);
    g_print("Max time: %d s\n", g_app_ctx.config.max_time_seconds);
    
    g_app_ctx.loop = g_main_loop_new(NULL, FALSE);
    
    if (!create_pipeline()) {
        g_printerr("Failed to create pipeline\n");
        cleanup();
        return -1;
    }
    
    g_print("Starting pipeline...\n");
    gst_element_set_state(g_app_ctx.pipeline, GST_STATE_PLAYING);
    g_main_loop_run(g_app_ctx.loop);
    
    g_print("Cleaning up...\n");
    cleanup();
    g_main_loop_unref(g_app_ctx.loop);
    
    return 0;
}
