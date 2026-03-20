/*
 * main.c — Entry point, Apostrophe initialisation, and app loop.
 *
 * Apostrophe implementation is included here (exactly once).
 */
#define AP_IMPLEMENTATION
#include "apostrophe.h"
#define AP_WIDGETS_IMPLEMENTATION
#include "apostrophe_widgets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "shortcuts.h"

/* ── Globals ──────────────────────────────────────────────────── */

bool g_is_brick = false;

/* ── App loop ─────────────────────────────────────────────────── */

static void run_app(void)
{
    for (;;) {
        main_action action = show_main_menu();
        switch (action) {
        case MAIN_ACTION_ADD_ROM:      add_rom_shortcut_flow();  break;
        case MAIN_ACTION_ADD_TOOL:     add_tool_shortcut_flow(); break;
        case MAIN_ACTION_MANAGE:       manage_shortcuts_flow();  break;
        case MAIN_ACTION_MANAGE_MEDIA: manage_media_flow();      break;
        case MAIN_ACTION_SETTINGS:     show_settings_screen();   break;
        case MAIN_ACTION_QUIT:         return;
        }
    }
}

/* ── Entry point ──────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    app_settings settings;

    if (argc > 1) {
        if (strcmp(argv[1], "--resume-sync-daemon") == 0)
            return resume_sync_daemon();
        if (strcmp(argv[1], "--resume-sync-once") == 0)
            return resume_sync_once() == 0 ? 0 : 1;
    }

    const char *dev = getenv("DEVICE");
    g_is_brick = (dev && strcasecmp(dev, "brick") == 0);

    ap_config cfg = {0};
    cfg.window_title = "Shortcuts";
    cfg.font_path    = AP_PLATFORM_IS_DEVICE ? NULL
                       : "third_party/apostrophe/res/font.ttf";
    cfg.log_path     = ap_resolve_log_path("shortcuts");
    cfg.is_nextui    = AP_PLATFORM_IS_DEVICE;
    cfg.cpu_speed    = AP_CPU_SPEED_MENU;

    if (ap_init(&cfg) != AP_OK) {
        fprintf(stderr, "Failed to initialise Apostrophe: %s\n",
                ap_get_error());
        return 1;
    }

    ap_log("startup: platform=%s is_brick=%d", AP_PLATFORM_NAME, g_is_brick);

    ensure_bridge_emu();
    settings = load_settings();
    if (AP_PLATFORM_IS_DEVICE) {
        if (set_resume_sync_autostart_enabled(settings.resume_sync_daemon) != 0)
            ap_log("startup: failed to update resume sync auto.sh");
        if (resume_sync_once() != 0)
            ap_log("startup: initial resume alias sync failed");
        if (settings.resume_sync_daemon)
            start_resume_sync_helper();
    }
    run_app();

    ap_quit();
    return 0;
}
