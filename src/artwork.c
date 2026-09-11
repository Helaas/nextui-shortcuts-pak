/*
 * artwork.c — PNG artwork compositing pipeline.
 *
 * Generates bg.png for shortcut folders by compositing:
 *   Layer 1: global wallpaper (or black canvas)
 *   Layer 2: game/tool artwork (right-aligned, rounded corners)
 *
 * Uses SDL2_image for PNG load/save and SDL2 surfaces for compositing.
 */
#include "shortcuts.h"

#ifndef TESTING
#include "apostrophe.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Helpers ──────────────────────────────────────────────────── */

#ifndef TESTING

/* Apply rounded corners to an RGBA surface (set pixels outside arcs to transparent).
 * Ports NextUI's GFX_ApplyRoundedCorners_8888. */
static void apply_rounded_corners(SDL_Surface *surf, int radius)
{
    int w = surf->w;
    int h = surf->h;
    if (radius <= 0 || w == 0 || h == 0) return;

    /* Clamp so overlapping corner circles can't erase the whole surface
     * (possible with a tiny artWidth setting and large corner radius). */
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    if (radius <= 0) return;

    SDL_LockSurface(surf);
    Uint32 *pixels = (Uint32 *)surf->pixels;
    int pitch = surf->pitch / 4; /* pitch in pixels (32-bit) */

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int dx = 0, dy = 0;
            if (x < radius)          dx = radius - x;
            else if (x >= w - radius) dx = x - (w - radius - 1);
            if (y < radius)          dy = radius - y;
            else if (y >= h - radius) dy = y - (h - radius - 1);

            if (dx * dx + dy * dy > radius * radius)
                pixels[y * pitch + x] = 0; /* fully transparent */
        }
    }
    SDL_UnlockSurface(surf);
}

/* Create a new RGBA surface. */
static SDL_Surface *create_rgba_surface(int w, int h)
{
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
    return SDL_CreateRGBSurface(0, w, h, 32,
                                0xFF000000, 0x00FF0000, 0x0000FF00, 0x000000FF);
#else
    return SDL_CreateRGBSurface(0, w, h, 32,
                                0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
#endif
}

/* Blit src scaled to cover dst (centre-crop, no letterbox). */
static void blit_cover(SDL_Surface *src, SDL_Surface *dst)
{
    double scaleX = (double)dst->w / src->w;
    double scaleY = (double)dst->h / src->h;
    double scale = scaleX > scaleY ? scaleX : scaleY;

    int newW = (int)(src->w * scale);
    int newH = (int)(src->h * scale);
    int offX = (newW - dst->w) / 2;
    int offY = (newH - dst->h) / 2;

    /* Source rect crops the centre portion of the scaled image. */
    SDL_Rect srcRect = {
        .x = (int)(offX / scale),
        .y = (int)(offY / scale),
        .w = (int)(dst->w / scale),
        .h = (int)(dst->h / scale),
    };
    SDL_BlitScaled(src, &srcRect, dst, NULL);
}

#endif /* !TESTING */

/* ── NextUI-matching art layout ───────────────────────────────── */

/* NextUI's FIXED_SCALE per platform (platform.h): scales UI metrics from
 * the 640x480 design base. tg5040 detects the Brick at runtime.
 * H700 also uses FIXED_SCALE=2, including on its 480-line panels. */
static int artwork_fixed_scale(void)
{
#ifndef TESTING
    if (ap_is_device()) {
        if (ap_get_platform() == AP_PLATFORM_MY355)
            return 1;
        if (ap_get_platform() == AP_PLATFORM_TG5040 && g_is_brick)
            return 3;
        return 2;
    }
#endif

    return g_is_brick ? 3 : 2;
}

typedef struct {
    int w, h;
    int x, y;
    int radius;
} art_layout;

/* Replicates NextUI's game-art thumbnail layout (nextui.c onThumbLoaded and
 * the LAYER_THUMBNAIL draw) so art baked into bg.png overlaps NextUI's own
 * thumbnail layer pixel-perfectly instead of ghosting next to it.
 *   max_w    = (int)(screen_w * artWidth)   max_h = (int)(screen_h * 0.6)
 *   margin   = SCALE1(BUTTON_MARGIN*3) = 15 * FIXED_SCALE
 *   center_y = (int)(screen_h * 0.5) - new_h/2
 *   radius   = thumbRadius * FIXED_SCALE (NextUI scales the radius by
 *              img_w/new_w before downscaling, which cancels out) */
static void compute_art_layout(int screen_w, int screen_h, int img_w, int img_h,
                               double art_width, int fixed_scale,
                               int thumb_radius, art_layout *out)
{
    int max_w = (int)(screen_w * art_width);
    int max_h = (int)(screen_h * 0.6);
    int new_w = max_w;
    int new_h = max_h;

    if (img_w > 0 && img_h > 0) {
        double aspect_ratio = (double)img_h / img_w;
        new_h = (int)(new_w * aspect_ratio);
        if (new_h > max_h) {
            new_h = max_h;
            new_w = (int)(new_h / aspect_ratio);
        }
    }

    out->w = new_w;
    out->h = new_h;
    out->x = screen_w - (new_w + 15 * fixed_scale);
    out->y = (int)(screen_h * 0.50) - new_h / 2;
    out->radius = thumb_radius * fixed_scale;
}

#ifdef TESTING
void compute_art_layout_for_tests(int screen_w, int screen_h,
                                  int img_w, int img_h,
                                  double art_width, int fixed_scale,
                                  int thumb_radius,
                                  int *out_w, int *out_h,
                                  int *out_x, int *out_y, int *out_radius)
{
    art_layout layout;
    compute_art_layout(screen_w, screen_h, img_w, img_h,
                       art_width, fixed_scale, thumb_radius, &layout);
    if (out_w) *out_w = layout.w;
    if (out_h) *out_h = layout.h;
    if (out_x) *out_x = layout.x;
    if (out_y) *out_y = layout.y;
    if (out_radius) *out_radius = layout.radius;
}
#endif

static uint32_t artwork_ticks(void)
{
#ifndef TESTING
    return SDL_GetTicks();
#else
    return 0;
#endif
}

static uint32_t elapsed_ms(uint32_t start_ms)
{
    return artwork_ticks() - start_ms;
}

static bool strip_suffix(char *str, const char *suffix)
{
    size_t str_len;
    size_t suffix_len;

    if (!str || !suffix) return false;

    str_len = strlen(str);
    suffix_len = strlen(suffix);
    if (suffix_len > str_len)
        return false;
    if (strcmp(str + str_len - suffix_len, suffix) != 0)
        return false;

    str[str_len - suffix_len] = '\0';
    return true;
}

static void strip_disabled_suffix(char *name)
{
    (void)strip_suffix(name, ".disabled");
}

/* ── Main compositing function ────────────────────────────────── */

#ifndef TESTING

void generate_artwork_bg(const char *art_src_path, const char *dest_folder,
                         bool use_global_bg, bool write_when_missing_art,
                         sc_color bg_color)
{
    uint32_t start_ms = artwork_ticks();
    uint32_t art_load_ms = 0;
    uint32_t bg_load_ms = 0;
    uint32_t compose_ms = 0;
    uint32_t save_ms = 0;
    struct stat st;
    SDL_Surface *art_img = NULL;
    uint32_t step_ms = SDL_GetTicks();

    if (stat(art_src_path, &st) == 0) {
        art_img = IMG_Load(art_src_path);
        art_load_ms = elapsed_ms(step_ms);
        if (!art_img) {
            ap_log("generate_artwork_bg: load art failed: %s (art=%ums total=%ums)",
                   IMG_GetError(), art_load_ms, elapsed_ms(start_ms));
            return;
        }
    } else if (!write_when_missing_art) {
        art_load_ms = elapsed_ms(step_ms);
        return; /* No art and this mode skips output when art is missing. */
    } else {
        art_load_ms = elapsed_ms(step_ms);
    }

    int screen_w, screen_h;
    get_screen_dimensions(&screen_w, &screen_h);

    /* Create canvas. */
    step_ms = SDL_GetTicks();
    SDL_Surface *canvas = create_rgba_surface(screen_w, screen_h);
    if (!canvas) {
        if (art_img) SDL_FreeSurface(art_img);
        return;
    }

    /* Fill with theme background color. */
    SDL_FillRect(canvas, NULL,
                 SDL_MapRGBA(canvas->format, bg_color.r, bg_color.g,
                             bg_color.b, bg_color.a));
    compose_ms += elapsed_ms(step_ms);

    /* Layer 1: global bg.png (scaled to cover, centre-crop). */
    if (use_global_bg) {
        char bg_path[SC_MAX_PATH];
        get_global_bg_path(bg_path, sizeof(bg_path));
        step_ms = SDL_GetTicks();
        SDL_Surface *bg_img = IMG_Load(bg_path);
        bg_load_ms = elapsed_ms(step_ms);
        if (bg_img) {
            step_ms = SDL_GetTicks();
            blit_cover(bg_img, canvas);
            compose_ms += elapsed_ms(step_ms);
            SDL_FreeSurface(bg_img);
        }
    }

    /* Layer 2: game/tool art thumbnail, positioned exactly like NextUI's
     * own LAYER_THUMBNAIL draw so the baked-in art and NextUI's thumbnail
     * layer overlap perfectly (no double-draw ghosting). */
    if (art_img) {
        step_ms = SDL_GetTicks();
        double art_width;
        int thumb_radius;
        get_nextui_art_settings(&art_width, &thumb_radius);

        art_layout layout;
        compute_art_layout(screen_w, screen_h, art_img->w, art_img->h,
                           art_width, artwork_fixed_scale(), thumb_radius,
                           &layout);

        /* Scale art to thumbnail size. */
        SDL_Surface *scaled_art = create_rgba_surface(layout.w, layout.h);
        if (scaled_art) {
            /* Set blend mode to none for the initial scale. */
            SDL_SetSurfaceBlendMode(art_img, SDL_BLENDMODE_NONE);
            SDL_BlitScaled(art_img, NULL, scaled_art, NULL);

            apply_rounded_corners(scaled_art, layout.radius);

            int target_x = layout.x;
            if (target_x < 0) target_x = 0;

            SDL_Rect dst_rect = { target_x, layout.y, layout.w, layout.h };
            SDL_SetSurfaceBlendMode(scaled_art, SDL_BLENDMODE_BLEND);
            SDL_BlitSurface(scaled_art, NULL, canvas, &dst_rect);

            SDL_FreeSurface(scaled_art);
        }
        compose_ms += elapsed_ms(step_ms);
        SDL_FreeSurface(art_img);
    }

    /* Save composite to .media/bg.png. */
    char media_dir[SC_MAX_PATH * 2];
    snprintf(media_dir, sizeof(media_dir), "%s/.media", dest_folder);
    step_ms = SDL_GetTicks();
    ensure_dir_exists(media_dir);

    char out_path[SC_MAX_PATH * 2 + sizeof("/bg.png")];
    snprintf(out_path, sizeof(out_path), "%s/bg.png", media_dir);
    int save_rc = IMG_SavePNG(canvas, out_path);
    save_ms = elapsed_ms(step_ms);
    SDL_FreeSurface(canvas);

    if (save_rc != 0) {
        ap_log("generate_artwork_bg: save failed: %s (art=%ums bg=%ums compose=%ums save=%ums total=%ums)",
               IMG_GetError(), art_load_ms, bg_load_ms, compose_ms, save_ms,
               elapsed_ms(start_ms));
        return;
    }

    ap_log("generate_artwork_bg: %s/.media/bg.png (%dx%d) art=%ums bg=%ums compose=%ums save=%ums total=%ums",
           dest_folder, screen_w, screen_h, art_load_ms, bg_load_ms,
           compose_ms, save_ms, elapsed_ms(start_ms));
}

#endif /* !TESTING */

/* ── Source art path resolution ────────────────────────────────── */

void shortcut_art_src_path(const shortcut_entry *sc, char *out, int out_size)
{
    out[0] = '\0';

    if (sc->is_tool) {
        /* Derive tool name from target path (e.g. ".../Retroarch.pak" → "Retroarch")
         * so artwork lookup works even if the shortcut was renamed.
         *
         * Be robust to multi-extension names like "Retroarch.pak.disabled":
         *   - strip a trailing ".pak.disabled" or ".pak" suffix if present
         *   - then strip any remaining generic extension (.exe, .sh, etc.) */
        char tools_dir[SC_MAX_PATH];
        get_tools_path(tools_dir, sizeof(tools_dir));

        const char *art_name = sc->display;
        char tool_name[SC_MAX_DISPLAY];
        if (sc->target_path[0] != '\0') {
            const char *base = strrchr(sc->target_path, '/');
            base = base ? base + 1 : sc->target_path;

            char base_name[SC_MAX_PATH];
            snprintf(base_name, sizeof(base_name), "%s", base);
            size_t len = strlen(base_name);

            /* Strip .pak.disabled or .pak suffix first. */
            if (ends_with(base_name, ".pak.disabled"))
                base_name[len - 13] = '\0';
            else if (ends_with(base_name, ".pak"))
                base_name[len - 4] = '\0';

            /* Then strip any remaining generic extension. */
            strip_extension(base_name, tool_name, sizeof(tool_name));
            art_name = tool_name;
        }
        snprintf(out, out_size, "%s/.media/%s.png", tools_dir, art_name);
        return;
    }

    /* Read the .m3u inside the shortcut folder to find the ROM's parent dir.
     * relPath is "../Console Dir (TAG)/[subfolder/.../]game.ext" — we need
     * everything up to the last '/' to locate the .media folder, and the
     * ROM filename to derive the artwork name (works even if renamed). */
    char m3u_path[SC_MAX_PATH + SC_MAX_NAME + 8];
    snprintf(m3u_path, sizeof(m3u_path), "%s/%s.m3u", sc->path, sc->name);
    char *data = read_text_file(m3u_path);
    if (!data) return;

    /* Trim trailing whitespace. */
    int len = (int)strlen(data);
    while (len > 0 && (data[len-1] == '\n' || data[len-1] == '\r' ||
                        data[len-1] == ' '))
        data[--len] = '\0';

    /* Parse: "../ConsoleDirName/[subfolder/.../]game.ext" */
    if (strncmp(data, "../", 3) != 0) { free(data); return; }

    const char *after = data + 3;
    const char *last_slash = strrchr(after, '/');
    if (!last_slash) { free(data); return; }

    /* rom_parent_rel = console dir + optional subfolders */
    char rom_parent_rel[SC_MAX_PATH];
    int plen = (int)(last_slash - after);
    if (plen >= (int)sizeof(rom_parent_rel)) plen = sizeof(rom_parent_rel) - 1;
    memcpy(rom_parent_rel, after, plen);
    rom_parent_rel[plen] = '\0';

    /* Derive artwork name from the ROM filename, not the shortcut display
     * name, so that artwork regeneration works for renamed shortcuts. */
    const char *rom_filename = last_slash + 1;
    char rom_filename_base[SC_MAX_DISPLAY];
    char rom_display[SC_MAX_DISPLAY];
    snprintf(rom_filename_base, sizeof(rom_filename_base), "%s", rom_filename);
    strip_disabled_suffix(rom_filename_base);
    strip_extension(rom_filename_base, rom_display, sizeof(rom_display));
    free(data);

    /* Multi-disc / CUE-folder fix: the .m3u relative path has an extra
     * directory level whose name matches the ROM display name.  Artwork
     * lives at the console level, not inside the subfolder. */
    char *parent_last_slash = strrchr(rom_parent_rel, '/');
    if (parent_last_slash) {
        char parent_leaf[SC_MAX_DISPLAY];
        snprintf(parent_leaf, sizeof(parent_leaf), "%s", parent_last_slash + 1);
        strip_disabled_suffix(parent_leaf);
        if (strcmp(parent_leaf, rom_display) == 0)
            *parent_last_slash = '\0';
    }

    char roms_dir[SC_MAX_PATH];
    get_roms_path(roms_dir, sizeof(roms_dir));
    snprintf(out, out_size, "%s/%s/.media/%s.png",
             roms_dir, rom_parent_rel, rom_display);
}

/* ── Bulk operations ──────────────────────────────────────────── */

int regenerate_all_media(const app_settings *settings)
{
    uint32_t start_ms = artwork_ticks();
    shortcut_entry *shortcuts = NULL;
    int count = 0;
    if (scan_shortcuts(&shortcuts, &count) != 0)
        return -1;

    bool use_bg, write_when_missing_art;
    artwork_bg_params(settings, &use_bg, &write_when_missing_art);
    sc_color bg_color = get_theme_bg_color();

    for (int i = 0; i < count; i++) {
        char art_src[SC_MAX_PATH];
        shortcut_art_src_path(&shortcuts[i], art_src, sizeof(art_src));
        generate_artwork_bg(art_src, shortcuts[i].path, use_bg,
                            write_when_missing_art, bg_color);
        if (sync_shortcut_thumbnail(shortcuts[i].name, art_src) != 0)
            ap_log("regenerate_all_media: thumbnail sync failed for %s",
                   shortcuts[i].name);
    }

    ap_log("regenerate_all_media: processed %d shortcuts in %ums",
           count, elapsed_ms(start_ms));
    free(shortcuts);
    return 0;
}

int remove_all_media(void)
{
    shortcut_entry *shortcuts = NULL;
    int count = 0;
    if (scan_shortcuts(&shortcuts, &count) != 0)
        return -1;

    for (int i = 0; i < count; i++) {
        char bg_path[SC_MAX_PATH * 2];
        snprintf(bg_path, sizeof(bg_path), "%s/.media/bg.png",
                 shortcuts[i].path);
        unlink(bg_path);

        /* Remove .media dir if empty. */
        char media_dir[SC_MAX_PATH * 2];
        snprintf(media_dir, sizeof(media_dir), "%s/.media",
                 shortcuts[i].path);
        rmdir(media_dir); /* Silently fails if not empty. */

        if (remove_shortcut_thumbnail(shortcuts[i].name) != 0)
            ap_log("remove_all_media: thumbnail remove failed for %s",
                   shortcuts[i].name);
    }

    ap_log("remove_all_media: processed %d shortcuts", count);
    free(shortcuts);
    return 0;
}
