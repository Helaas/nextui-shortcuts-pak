#include <stdbool.h>

#include "shortcuts.h"

bool g_is_brick = false;

void ap_log(const char *fmt, ...)
{
    (void)fmt;
}

void generate_artwork_bg(const char *art_src_path, const char *dest_folder,
                         bool use_global_bg, bool write_when_missing_art)
{
    (void)art_src_path;
    (void)dest_folder;
    (void)use_global_bg;
    (void)write_when_missing_art;
}
