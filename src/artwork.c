/*
 * artwork.c — PNG artwork compositing pipeline.
 *
 * Generates bg.png for shortcut folders by compositing:
 *   Layer 1: global wallpaper (or black canvas)
 *   Layer 2: game/tool artwork (right-aligned, rounded corners)
 *
 * Uses SDL2_image for PNG load/save and SDL2 surfaces for compositing.
 */
#include "apostrophe.h"
#include "shortcuts.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* ── Helpers ──────────────────────────────────────────────────── */

/* Scale (srcW, srcH) to fit within (maxW, maxH) preserving aspect ratio. */
static void thumbnail_fit(int srcW, int srcH, int maxW, int maxH,
                          int *outW, int *outH)
{
    if (srcW <= 0 || srcH <= 0) {
        *outW = maxW; *outH = maxH;
        return;
    }
    *outW = maxW;
    *outH = srcH * maxW / srcW;
    if (*outH > maxH) {
        *outH = maxH;
        *outW = srcW * maxH / srcH;
    }
}

/* Apply rounded corners to an RGBA surface (set pixels outside arcs to transparent).
 * Ports NextUI's GFX_ApplyRoundedCorners_8888. */
static void apply_rounded_corners(SDL_Surface *surf, int radius)
{
    int w = surf->w;
    int h = surf->h;
    if (radius <= 0 || w == 0 || h == 0) return;

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

/* ── Main compositing function ────────────────────────────────── */

void generate_artwork_bg(const char *art_src_path, const char *dest_folder,
                         bool use_global_bg, bool force_black)
{
    struct stat st;
    SDL_Surface *art_img = NULL;

    if (stat(art_src_path, &st) == 0) {
        art_img = IMG_Load(art_src_path);
        if (!art_img) {
            ap_log("generate_artwork_bg: load art failed: %s", IMG_GetError());
            return;
        }
    } else if (!force_black) {
        return; /* No art and not forcing — skip. */
    }

    int screen_w, screen_h;
    get_screen_dimensions(&screen_w, &screen_h);

    /* Create canvas. */
    SDL_Surface *canvas = create_rgba_surface(screen_w, screen_h);
    if (!canvas) {
        if (art_img) SDL_FreeSurface(art_img);
        return;
    }

    /* Fill with opaque black. */
    SDL_FillRect(canvas, NULL,
                 SDL_MapRGBA(canvas->format, 0, 0, 0, 255));

    /* Layer 1: global bg.png (scaled to cover, centre-crop). */
    if (use_global_bg) {
        char bg_path[SC_MAX_PATH];
        get_global_bg_path(bg_path, sizeof(bg_path));
        SDL_Surface *bg_img = IMG_Load(bg_path);
        if (bg_img) {
            blit_cover(bg_img, canvas);
            SDL_FreeSurface(bg_img);
        }
    }

    /* Layer 2: game/tool art thumbnail, right-aligned with rounded corners.
     *   max_w = screen_w * 0.45
     *   max_h = screen_h * 0.60
     *   right margin = 30 px (SCALE1(BUTTON_MARGIN*3) at FIXED_SCALE=2)
     *   vertically centred at screen_h/2
     */
    if (art_img) {
        int maxW = (int)(screen_w * 0.45);
        int maxH = (int)(screen_h * 0.60);
        int artW, artH;
        thumbnail_fit(art_img->w, art_img->h, maxW, maxH, &artW, &artH);

        /* Scale art to thumbnail size. */
        SDL_Surface *scaled_art = create_rgba_surface(artW, artH);
        if (scaled_art) {
            /* Set blend mode to none for the initial scale. */
            SDL_SetSurfaceBlendMode(art_img, SDL_BLENDMODE_NONE);
            SDL_BlitScaled(art_img, NULL, scaled_art, NULL);

            /* Rounded corners (radius = 40 px). */
            apply_rounded_corners(scaled_art, 40);

            /* Right-align with margin, vertically centre. */
            int right_margin = 30;
            int target_x = screen_w - artW - right_margin;
            if (target_x < 0) target_x = 0;
            int center_y = screen_h / 2 - artH / 2;

            SDL_Rect dst_rect = { target_x, center_y, artW, artH };
            SDL_SetSurfaceBlendMode(scaled_art, SDL_BLENDMODE_BLEND);
            SDL_BlitSurface(scaled_art, NULL, canvas, &dst_rect);

            SDL_FreeSurface(scaled_art);
        }
        SDL_FreeSurface(art_img);
    }

    /* Save composite to .media/bg.png. */
    char media_dir[SC_MAX_PATH];
    snprintf(media_dir, sizeof(media_dir), "%s/.media", dest_folder);
    ensure_dir_exists(media_dir);

    char out_path[SC_MAX_PATH * 2];
    snprintf(out_path, sizeof(out_path), "%s/bg.png", media_dir);
    IMG_SavePNG(canvas, out_path);
    SDL_FreeSurface(canvas);

    ap_log("generate_artwork_bg: %s/.media/bg.png (%dx%d)",
           dest_folder, screen_w, screen_h);
}

/* ── Source art path resolution ────────────────────────────────── */

void shortcut_art_src_path(const shortcut_entry *sc, char *out, int out_size)
{
    out[0] = '\0';

    if (sc->is_tool) {
        char tools_dir[SC_MAX_PATH];
        get_tools_path(tools_dir, sizeof(tools_dir));
        snprintf(out, out_size, "%s/.media/%s.png", tools_dir, sc->display);
        return;
    }

    /* Read the .m3u inside the shortcut folder to find the console directory.
     * relPath is "../Console Dir (TAG)/game.rom" — second component is the
     * console dir. */
    char m3u_path[SC_MAX_PATH + SC_MAX_NAME + 8];
    snprintf(m3u_path, sizeof(m3u_path), "%s/%s.m3u", sc->path, sc->name);
    char *data = read_text_file(m3u_path);
    if (!data) return;

    /* Trim trailing whitespace. */
    int len = (int)strlen(data);
    while (len > 0 && (data[len-1] == '\n' || data[len-1] == '\r' ||
                        data[len-1] == ' '))
        data[--len] = '\0';

    /* Parse: "../ConsoleDirName/game.ext" */
    if (strncmp(data, "../", 3) != 0) { free(data); return; }

    const char *after = data + 3;
    const char *slash = strchr(after, '/');
    if (!slash) { free(data); return; }

    char console_dir_name[SC_MAX_NAME];
    int clen = (int)(slash - after);
    if (clen >= (int)sizeof(console_dir_name)) clen = sizeof(console_dir_name) - 1;
    memcpy(console_dir_name, after, clen);
    console_dir_name[clen] = '\0';
    free(data);

    char roms_dir[SC_MAX_PATH];
    get_roms_path(roms_dir, sizeof(roms_dir));
    snprintf(out, out_size, "%s/%s/.media/%s.png",
             roms_dir, console_dir_name, sc->display);
}

/* ── Bulk operations ──────────────────────────────────────────── */

int regenerate_all_media(const app_settings *settings)
{
    shortcut_entry *shortcuts = NULL;
    int count = 0;
    if (scan_shortcuts(&shortcuts, &count) != 0)
        return -1;

    bool use_bg, force_blk;
    artwork_bg_params(settings, &use_bg, &force_blk);

    for (int i = 0; i < count; i++) {
        char art_src[SC_MAX_PATH];
        shortcut_art_src_path(&shortcuts[i], art_src, sizeof(art_src));
        generate_artwork_bg(art_src, shortcuts[i].path, use_bg, force_blk);
    }

    ap_log("regenerate_all_media: processed %d shortcuts", count);
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
    }

    ap_log("remove_all_media: processed %d shortcuts", count);
    free(shortcuts);
    return 0;
}
