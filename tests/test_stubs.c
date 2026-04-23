#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "shortcuts.h"

bool g_is_brick = false;
char g_last_generated_art_src_path[SC_MAX_PATH * 2];
char g_last_generated_art_dest_folder[SC_MAX_PATH * 2];
int g_generate_artwork_bg_call_count = 0;

void ap_log(const char *fmt, ...)
{
    (void)fmt;
}

void reset_generate_artwork_bg_stub(void)
{
    g_last_generated_art_src_path[0] = '\0';
    g_last_generated_art_dest_folder[0] = '\0';
    g_generate_artwork_bg_call_count = 0;
}

void generate_artwork_bg(const char *art_src_path, const char *dest_folder,
                         bool use_global_bg, bool write_when_missing_art,
                         sc_color bg_color)
{
    (void)use_global_bg;
    (void)write_when_missing_art;
    (void)bg_color;

    g_generate_artwork_bg_call_count++;
    if (art_src_path) {
        snprintf(g_last_generated_art_src_path,
                 sizeof(g_last_generated_art_src_path),
                 "%s", art_src_path);
    } else {
        g_last_generated_art_src_path[0] = '\0';
    }
    if (dest_folder) {
        snprintf(g_last_generated_art_dest_folder,
                 sizeof(g_last_generated_art_dest_folder),
                 "%s", dest_folder);
    } else {
        g_last_generated_art_dest_folder[0] = '\0';
    }
}
