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
#include <stdio.h>
#include <string.h>

#include "editor.h"

static sg_pass_action pass_action;
static Editor* editor;
static char startup_level[512];

static void init(void) {
    sg_setup(&(sg_desc){
        .environment = sglue_environment(),
        .logger.func = slog_func,
    });
    simgui_setup(&(simgui_desc_t){
        .logger.func = slog_func,
        .no_default_font = true,
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
    simgui_handle_event(ev);
}

sapp_desc sokol_main(int argc, char* argv[]) {
    snprintf(startup_level, sizeof startup_level, "%s", "levels/re2-library.lvl");
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--level") == 0) {
            snprintf(startup_level, sizeof startup_level, "%s", argv[i + 1]);
            break;
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
        .logger.func = slog_func,
    };
}
