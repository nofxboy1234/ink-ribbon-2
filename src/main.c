#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_log.h"
#include "sokol_glue.h"
#include "cimgui.h"

#define SOKOL_IMPL
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_log.h"
#include "sokol_glue.h"
#define SOKOL_IMGUI_IMPL
#include "sokol_imgui.h"

#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__linux__) && !defined(__ANDROID__)
#include <X11/Xlib.h>
#endif

#include "editor.h"

static sg_pass_action pass_action;
static Editor* editor;
static char startup_level[512];

/* sokol has no windowed "maximized" flag. On X11, fill the screen and ask the
   window manager to mark the window maximized, so a decorated desktop also
   keeps it snapped to the work area. */
static void start_window_maximized(void) {
#if defined(__linux__) && !defined(__ANDROID__)
    Display* display = (Display*)sapp_x11_get_display();
    Window window = (Window)(uintptr_t)sapp_x11_get_window();
    if (!display || !window) return;
    int screen = DefaultScreen(display);
    int width = DisplayWidth(display, screen);
    int height = DisplayHeight(display, screen);
    XMoveResizeWindow(display, window, 0, 0, (unsigned)width, (unsigned)height);
    Atom state = XInternAtom(display, "_NET_WM_STATE", False);
    Atom max_vert = XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    Atom max_horz = XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    if (state && max_vert && max_horz) {
        XEvent event;
        memset(&event, 0, sizeof event);
        event.type = ClientMessage;
        event.xclient.window = window;
        event.xclient.message_type = state;
        event.xclient.format = 32;
        event.xclient.data.l[0] = 1; /* _NET_WM_STATE_ADD */
        event.xclient.data.l[1] = (long)max_vert;
        event.xclient.data.l[2] = (long)max_horz;
        event.xclient.data.l[3] = 1; /* normal application source */
        XSendEvent(display, DefaultRootWindow(display), False,
                   SubstructureNotifyMask | SubstructureRedirectMask, &event);
    }
    XFlush(display);
#endif
}

static void init(void) {
    start_window_maximized();
    sg_setup(&(sg_desc){
        .environment = sglue_environment(),
        .logger.func = slog_func,
    });
    simgui_setup(&(simgui_desc_t){
        .logger.func = slog_func,
        .no_default_font = true,
        /* Ctrl+V belongs to the editor's object clipboard, not the system
           clipboard. Keep sokol from swallowing it as a text paste. */
        .disable_paste_override = true,
        /* The edit-mode nav-cell overlay can add one filled/bordered rect per
           open cell; 1F is large enough to exceed the 64k default. */
        .max_vertices = 524288,
    });

    ImFontConfig font_config = { 0 };
    font_config.SizePixels = 15.0f;
    font_config.FontDataOwnedByAtlas = true;
    font_config.GlyphMaxAdvanceX = FLT_MAX;
    font_config.RasterizerMultiply = 1.0f;
    font_config.RasterizerDensity = 1.0f;
    font_config.ExtraSizeScale = 1.0f;
    ImFontAtlas_AddFontDefaultVector(igGetIO()->Fonts, &font_config);

    editor = editor_create(startup_level[0] ? startup_level : "levels/re2-library.lvl");

    pass_action.colors[0].load_action = SG_LOADACTION_CLEAR;
    pass_action.colors[0].clear_value = (sg_color){ 0.043f, 0.047f, 0.055f, 1.0f };
}

static void frame(void) {
    simgui_new_frame(&(simgui_frame_desc_t){
        .width = sapp_width(),
        .height = sapp_height(),
        .delta_time = sapp_frame_duration(),
        .dpi_scale = sapp_dpi_scale(),
    });
    if (editor) editor_frame(editor);
    simgui_flush();
    sg_begin_pass(&(sg_pass){ .action = pass_action, .swapchain = sglue_swapchain() });
    simgui_draw();
    sg_end_pass();
    sg_commit();
}

static void cleanup(void) {
    editor_destroy(editor);
    editor = NULL;
    simgui_shutdown();
    sg_shutdown();
}

static void input(const sapp_event* ev) {
    if (editor && ev->type == SAPP_EVENTTYPE_FILES_DROPPED) {
        int count = sapp_get_num_dropped_files();
        for (int i = 0; i < count; i++) {
            const char* path = sapp_get_dropped_file_path(i);
            if (path && path[0]) {
                editor_load_onion_image(editor, path);
                break;
            }
        }
    }
    simgui_handle_event(ev);
}

sapp_desc sokol_main(int argc, char* argv[]) {
    snprintf(startup_level, sizeof startup_level, "%s", "levels/re2-library.lvl");
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--level") == 0 && i + 1 < argc) {
            snprintf(startup_level, sizeof startup_level, "%s", argv[i + 1]);
            i++;
        }
    }

    return (sapp_desc){
        .init_cb = init,
        .frame_cb = frame,
        .cleanup_cb = cleanup,
        .event_cb = input,
        .window_title = "Ink Ribbon Level Editor",
        .width = 1280,
        .height = 800,
        .enable_dragndrop = true,
        .max_dropped_files = 4,
        .max_dropped_file_path_length = 4096,
        .logger.func = slog_func,
    };
}
