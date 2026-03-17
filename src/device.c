/*
 * device.c — Core logic: string utilities, path resolution, settings,
 *            filesystem scanning, shortcut CRUD, and bridge emu management.
 */
#include "apostrophe.h"
#include "shortcuts.h"
#include "cjson/cjson.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── String utilities ─────────────────────────────────────────── */

bool starts_with(const char *str, const char *prefix)
{
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

bool ends_with(const char *str, const char *suffix)
{
    size_t slen = strlen(str);
    size_t xlen = strlen(suffix);
    if (xlen > slen) return false;
    return strcmp(str + slen - xlen, suffix) == 0;
}

/* extract_tag: "Game Boy Advance (GBA)" -> "GBA" */
void extract_tag(const char *name, char *out, int out_size)
{
    out[0] = '\0';
    const char *open = NULL;
    const char *close = NULL;
    /* Find the LAST '(' and ')' pair. */
    for (const char *p = name; *p; p++) {
        if (*p == '(') open = p;
        if (*p == ')') close = p;
    }
    if (!open || !close || close <= open) return;

    int len = (int)(close - open - 1);
    if (len <= 0 || len >= out_size) return;

    /* Copy and trim whitespace. */
    const char *start = open + 1;
    while (len > 0 && *start == ' ') { start++; len--; }
    while (len > 0 && *(start + len - 1) == ' ') len--;

    if (len > 0 && len < out_size) {
        memcpy(out, start, len);
        out[len] = '\0';
    }
}

/* extract_display_name: "Game Boy Advance (GBA)" -> "Game Boy Advance" */
void extract_display_name(const char *name, char *out, int out_size)
{
    const char *open = NULL;
    for (const char *p = name; *p; p++) {
        if (*p == '(') open = p;
    }
    if (!open) {
        snprintf(out, out_size, "%s", name);
        return;
    }
    int len = (int)(open - name);
    /* Trim trailing whitespace. */
    while (len > 0 && name[len - 1] == ' ') len--;
    if (len <= 0) {
        snprintf(out, out_size, "%s", name);
        return;
    }
    if (len >= out_size) len = out_size - 1;
    memcpy(out, name, len);
    out[len] = '\0';
}

/* strip_extension: "game.sfc" -> "game" (extensions 2-5 chars) */
void strip_extension(const char *name, char *out, int out_size)
{
    const char *dot = strrchr(name, '.');
    if (dot && dot != name) {
        int ext_len = (int)strlen(dot);
        if (ext_len >= 2 && ext_len <= 5) {
            int base_len = (int)(dot - name);
            if (base_len >= out_size) base_len = out_size - 1;
            memcpy(out, name, base_len);
            out[base_len] = '\0';
            return;
        }
    }
    snprintf(out, out_size, "%s", name);
}

/* build_folder_name: construct prefixed shortcut folder name. */
void build_folder_name(sc_position pos, const char *display, const char *tag,
                       char *out, int out_size)
{
    switch (pos) {
    case SC_POS_TOP:
        snprintf(out, out_size, TOP_PREFIX "%s (%s)", display, tag);
        break;
    case SC_POS_ALPHA:
        snprintf(out, out_size, "%s (%s)", display, tag);
        break;
    default: /* SC_POS_BOTTOM */
        snprintf(out, out_size, SHORTCUT_PREFIX "%s (%s)", display, tag);
        break;
    }
}

bool is_hidden(const char *name)
{
    return name[0] == '.' ||
           ends_with(name, ".disabled") ||
           strcmp(name, "map.txt") == 0;
}

bool is_mac_dotfile(const char *name)
{
    if (name[0] != '.') return false;
    char tag[SC_MAX_TAG];
    extract_tag(name, tag, sizeof(tag));
    return tag[0] == '\0';
}

bool is_shortcut_folder(const char *folder_path)
{
    const char *name = strrchr(folder_path, '/');
    name = name ? name + 1 : folder_path;

    if (starts_with(name, SHORTCUT_PREFIX) ||
        starts_with(name, LEGACY_PREFIX)) {
        return true;
    }

    /* Check for .shortcut marker file. */
    char marker[SC_MAX_PATH];
    snprintf(marker, sizeof(marker), "%s/" SHORTCUT_MARKER, folder_path);
    struct stat st;
    return stat(marker, &st) == 0;
}

bool dir_has_visible_content(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return false;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (!is_hidden(ent->d_name)) {
            closedir(d);
            return true;
        }
    }
    closedir(d);
    return false;
}

/* ── File I/O utilities ───────────────────────────────────────── */

/* Read an entire file into a malloc'd string. Returns NULL on failure. */
char *read_text_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); return NULL; }
    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, len, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

int write_text_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(content, f);
    fclose(f);
    return 0;
}

int ensure_dir_exists(const char *path)
{
    char tmp[SC_MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

int rmdir_recursive(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char child[SC_MAX_PATH];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        struct stat st;
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            rmdir_recursive(child);
        } else {
            unlink(child);
        }
    }
    closedir(d);
    return rmdir(path);
}

/* ── Path resolution ──────────────────────────────────────────── */

#if defined(PLATFORM_MAC)
    #define PLATFORM_SUBDIR "tg5040"
#elif defined(PLATFORM_TG5050)
    #define PLATFORM_SUBDIR "tg5050"
#elif defined(PLATFORM_MY355)
    #define PLATFORM_SUBDIR "my355"
#else /* TG5040 */
    #define PLATFORM_SUBDIR "tg5040"
#endif

void get_roms_path(char *out, int out_size)
{
#if defined(PLATFORM_MAC)
    char cwd[SC_MAX_PATH];
    if (getcwd(cwd, sizeof(cwd)))
        snprintf(out, out_size, "%s/mock_sdcard/Roms", cwd);
    else
        snprintf(out, out_size, "./mock_sdcard/Roms");
#else
    const char *sd = getenv("SDCARD_PATH");
    snprintf(out, out_size, "%s/Roms", sd && sd[0] ? sd : "/mnt/SDCARD");
#endif
}

void get_tools_path(char *out, int out_size)
{
#if defined(PLATFORM_MAC)
    char cwd[SC_MAX_PATH];
    if (getcwd(cwd, sizeof(cwd)))
        snprintf(out, out_size, "%s/mock_sdcard/Tools/" PLATFORM_SUBDIR, cwd);
    else
        snprintf(out, out_size, "./mock_sdcard/Tools/" PLATFORM_SUBDIR);
#else
    const char *sd = getenv("SDCARD_PATH");
    snprintf(out, out_size, "%s/Tools/" PLATFORM_SUBDIR,
             sd && sd[0] ? sd : "/mnt/SDCARD");
#endif
}

void get_emus_path(char *out, int out_size)
{
#if defined(PLATFORM_MAC)
    char cwd[SC_MAX_PATH];
    if (getcwd(cwd, sizeof(cwd)))
        snprintf(out, out_size, "%s/mock_sdcard/Emus/" PLATFORM_SUBDIR, cwd);
    else
        snprintf(out, out_size, "./mock_sdcard/Emus/" PLATFORM_SUBDIR);
#else
    const char *sd = getenv("SDCARD_PATH");
    snprintf(out, out_size, "%s/Emus/" PLATFORM_SUBDIR,
             sd && sd[0] ? sd : "/mnt/SDCARD");
#endif
}

void get_settings_path(char *out, int out_size)
{
#if defined(PLATFORM_MAC)
    char cwd[SC_MAX_PATH];
    if (getcwd(cwd, sizeof(cwd)))
        snprintf(out, out_size,
                 "%s/mock_sdcard/.userdata/shared/Shortcuts/settings.json", cwd);
    else
        snprintf(out, out_size,
                 "./mock_sdcard/.userdata/shared/Shortcuts/settings.json");
#else
    const char *sd = getenv("SDCARD_PATH");
    snprintf(out, out_size, "%s/.userdata/shared/Shortcuts/settings.json",
             sd && sd[0] ? sd : "/mnt/SDCARD");
#endif
}

void get_global_bg_path(char *out, int out_size)
{
#if defined(PLATFORM_MAC)
    char cwd[SC_MAX_PATH];
    if (getcwd(cwd, sizeof(cwd)))
        snprintf(out, out_size, "%s/mock_sdcard/bg.png", cwd);
    else
        snprintf(out, out_size, "./mock_sdcard/bg.png");
#else
    const char *sd = getenv("SDCARD_PATH");
    snprintf(out, out_size, "%s/bg.png",
             sd && sd[0] ? sd : "/mnt/SDCARD");
#endif
}

void get_screen_dimensions(int *w, int *h)
{
#if defined(PLATFORM_MY355)
    *w = 640; *h = 480;
#elif defined(PLATFORM_TG5040) || defined(PLATFORM_MAC)
    if (g_is_brick) { *w = 1024; *h = 768; }
    else            { *w = 1280; *h = 720; }
#else /* TG5050 */
    *w = 1280; *h = 720;
#endif
}

/* ── Settings (JSON via cJSON) ────────────────────────────────── */

app_settings load_settings(void)
{
    app_settings s = { .copy_artwork = true, .artwork_mode = ART_MODE_WALLPAPER,
                       .show_hidden = false };
    char path[SC_MAX_PATH];
    get_settings_path(path, sizeof(path));
    char *data = read_text_file(path);
    if (!data) return s;

    cJSON *json = cJSON_Parse(data);
    free(data);
    if (!json) return s;

    cJSON *ca = cJSON_GetObjectItem(json, "copy_artwork");
    if (cJSON_IsBool(ca)) s.copy_artwork = cJSON_IsTrue(ca);

    cJSON *am = cJSON_GetObjectItem(json, "artwork_mode");
    if (cJSON_IsNumber(am)) s.artwork_mode = (art_mode)am->valueint;

    cJSON *sh = cJSON_GetObjectItem(json, "show_hidden");
    if (cJSON_IsBool(sh)) s.show_hidden = cJSON_IsTrue(sh);

    cJSON_Delete(json);
    return s;
}

int save_settings(const app_settings *s)
{
    char path[SC_MAX_PATH];
    get_settings_path(path, sizeof(path));

    /* Ensure parent directory exists. */
    char dir[SC_MAX_PATH];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        ensure_dir_exists(dir);
    }

    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "copy_artwork", s->copy_artwork);
    cJSON_AddNumberToObject(json, "artwork_mode", (int)s->artwork_mode);
    cJSON_AddBoolToObject(json, "show_hidden", s->show_hidden);

    char *str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!str) return -1;

    int rc = write_text_file(path, str);
    free(str);
    return rc;
}

void artwork_bg_params(const app_settings *s, bool *use_global_bg,
                       bool *force_black)
{
    switch (s->artwork_mode) {
    case ART_MODE_WALLPAPER:
        *use_global_bg = true;  *force_black = true;  break;
    case ART_MODE_FALLBACK:
        *use_global_bg = true;  *force_black = false; break;
    default: /* ART_MODE_BLACK */
        *use_global_bg = false; *force_black = true;  break;
    }
}

/* ── Shortcut marker helpers ──────────────────────────────────── */

static char *read_shortcut_marker(const char *folder_path)
{
    char marker[SC_MAX_PATH * 2];
    snprintf(marker, sizeof(marker), "%s/" SHORTCUT_MARKER, folder_path);
    char *data = read_text_file(marker);
    if (!data) return NULL;
    /* Trim trailing whitespace. */
    int len = (int)strlen(data);
    while (len > 0 && (data[len-1] == ' ' || data[len-1] == '\n' ||
                        data[len-1] == '\r' || data[len-1] == '\t'))
        data[--len] = '\0';
    if (len == 0) { free(data); return NULL; }
    return data;
}

static int write_shortcut_marker(const char *folder_path,
                                 const char *display_name)
{
    char marker[SC_MAX_PATH * 2];
    snprintf(marker, sizeof(marker), "%s/" SHORTCUT_MARKER, folder_path);
    return write_text_file(marker, display_name);
}

static void normalize_path(const char *path, char *out, int out_size)
{
    char tmp[SC_MAX_PATH];
    char *parts[SC_MAX_PATH / 2];
    int count = 0;
    bool absolute;

    if (!path || !out || out_size <= 0) return;

    snprintf(tmp, sizeof(tmp), "%s", path);
    absolute = (tmp[0] == '/');
    out[0] = '\0';

    char *saveptr = NULL;
    for (char *token = strtok_r(tmp, "/", &saveptr);
         token != NULL;
         token = strtok_r(NULL, "/", &saveptr)) {
        if (token[0] == '\0' || strcmp(token, ".") == 0)
            continue;

        if (strcmp(token, "..") == 0) {
            if (count > 0 && strcmp(parts[count - 1], "..") != 0) {
                count--;
            } else if (!absolute) {
                parts[count++] = token;
            }
            continue;
        }

        parts[count++] = token;
    }

    if (absolute)
        snprintf(out, out_size, "/");

    for (int i = 0; i < count; i++) {
        size_t len = strlen(out);
        if (len > 0 && out[len - 1] != '/')
            strncat(out, "/", out_size - strlen(out) - 1);
        strncat(out, parts[i], out_size - strlen(out) - 1);
    }

    if (out[0] == '\0')
        snprintf(out, out_size, absolute ? "/" : ".");
}

/* ── Scanning — comparison function ───────────────────────────── */

static int cmp_console_dir(const void *a, const void *b)
{
    return strcasecmp(((const console_dir *)a)->display,
                      ((const console_dir *)b)->display);
}

static int cmp_rom_file(const void *a, const void *b)
{
    return strcasecmp(((const rom_file *)a)->display,
                      ((const rom_file *)b)->display);
}

static int cmp_tool_pak(const void *a, const void *b)
{
    return strcasecmp(((const tool_pak *)a)->display,
                      ((const tool_pak *)b)->display);
}

static int cmp_shortcut_entry(const void *a, const void *b)
{
    return strcasecmp(((const shortcut_entry *)a)->display,
                      ((const shortcut_entry *)b)->display);
}

/* ── Dynamic array helper ─────────────────────────────────────── */

/* Grow a realloc'd array. Returns new pointer or NULL on failure. */
static void *grow_array(void *arr, int *cap, int count, size_t elem_size)
{
    if (count < *cap) return arr;
    int new_cap = *cap == 0 ? 64 : *cap * 2;
    void *new_arr = realloc(arr, new_cap * elem_size);
    if (new_arr) *cap = new_cap;
    return new_arr;
}

/* ── Scanning functions ───────────────────────────────────────── */

int scan_console_dirs(bool show_hidden, console_dir **out, int *count)
{
    char roms_dir[SC_MAX_PATH];
    get_roms_path(roms_dir, sizeof(roms_dir));

    DIR *d = opendir(roms_dir);
    if (!d) { *out = NULL; *count = 0; return -1; }

    console_dir *arr = NULL;
    int n = 0, cap = 0;
    struct dirent *ent;

    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        /* Must be a directory. */
        char full[SC_MAX_PATH * 2];
        snprintf(full, sizeof(full), "%s/%s", roms_dir, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;

        if (is_shortcut_folder(full))
            continue;

        const char *name = ent->d_name;

        if (!show_hidden) {
            if (is_hidden(name)) continue;
            if (!dir_has_visible_content(full)) continue;
        } else {
            if (is_mac_dotfile(name) || strcmp(name, "map.txt") == 0) continue;
        }

        /* Strip .disabled suffix for tag/display extraction. */
        bool disabled = ends_with(name, ".disabled");
        char base_name[SC_MAX_NAME];
        snprintf(base_name, sizeof(base_name), "%s", name);
        if (disabled) {
            base_name[strlen(base_name) - strlen(".disabled")] = '\0';
        }

        char tag[SC_MAX_TAG];
        extract_tag(base_name, tag, sizeof(tag));
        if (tag[0] == '\0') continue; /* no emu tag — skip */

        arr = grow_array(arr, &cap, n, sizeof(console_dir));
        if (!arr) { closedir(d); *out = NULL; *count = 0; return -1; }

        console_dir *c = &arr[n++];
        memset(c, 0, sizeof(*c));
        snprintf(c->name, sizeof(c->name), "%s", name);
        snprintf(c->tag, sizeof(c->tag), "%s", tag);
        snprintf(c->path, sizeof(c->path), "%s", full);
        extract_display_name(base_name, c->display, sizeof(c->display));
        c->is_disabled = disabled;
    }
    closedir(d);

    if (n > 1) qsort(arr, n, sizeof(console_dir), cmp_console_dir);

    ap_log("scan_console_dirs: show_hidden=%d found=%d", show_hidden, n);
    *out = arr;
    *count = n;
    return 0;
}

/* Internal recursive ROM scanner. */
static int scan_roms_internal(const char *dir_path, bool show_hidden,
                              rom_file **arr, int *n, int *cap)
{
    DIR *d = opendir(dir_path);
    if (!d) return -1;
    struct dirent *ent;

    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        const char *name = ent->d_name;

        if (!show_hidden) {
            if (is_hidden(name)) continue;
        } else {
            if (name[0] == '.' || strcmp(name, "map.txt") == 0) continue;
        }

        bool disabled = ends_with(name, ".disabled");
        char base_name[SC_MAX_NAME];
        snprintf(base_name, sizeof(base_name), "%s", name);
        if (disabled)
            base_name[strlen(base_name) - strlen(".disabled")] = '\0';

        char full[SC_MAX_PATH];
        snprintf(full, sizeof(full), "%s/%s", dir_path, name);
        struct stat st;
        if (stat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            /* Check for multi-disc: {baseName}.m3u inside subfolder. */
            char check[SC_MAX_PATH * 2];
            snprintf(check, sizeof(check), "%s/%s.m3u", full, base_name);
            struct stat m3u_st;
            if (stat(check, &m3u_st) == 0) {
                *arr = grow_array(*arr, cap, *n, sizeof(rom_file));
                if (!*arr) { closedir(d); return -1; }
                rom_file *r = &(*arr)[(*n)++];
                memset(r, 0, sizeof(*r));
                snprintf(r->name, sizeof(r->name), "%s", name);
                snprintf(r->path, sizeof(r->path), "%s", full);
                snprintf(r->display, sizeof(r->display), "%s", base_name);
                r->is_multi_disc = true;
                r->is_disabled = disabled;
                continue;
            }

            /* Check for CUE folder: {baseName}.cue inside subfolder. */
            snprintf(check, sizeof(check), "%s/%s.cue", full, base_name);
            if (stat(check, &m3u_st) == 0) {
                *arr = grow_array(*arr, cap, *n, sizeof(rom_file));
                if (!*arr) { closedir(d); return -1; }
                rom_file *r = &(*arr)[(*n)++];
                memset(r, 0, sizeof(*r));
                snprintf(r->name, sizeof(r->name), "%s", name);
                snprintf(r->path, sizeof(r->path), "%s", full);
                snprintf(r->display, sizeof(r->display), "%s", base_name);
                r->is_cue_folder = true;
                r->is_disabled = disabled;
                continue;
            }

            /* Plain subfolder — recurse. */
            scan_roms_internal(full, show_hidden, arr, n, cap);
            continue;
        }

        /* Regular file. */
        *arr = grow_array(*arr, cap, *n, sizeof(rom_file));
        if (!*arr) { closedir(d); return -1; }
        rom_file *r = &(*arr)[(*n)++];
        memset(r, 0, sizeof(*r));
        snprintf(r->name, sizeof(r->name), "%s", name);
        snprintf(r->path, sizeof(r->path), "%s", full);
        strip_extension(base_name, r->display, sizeof(r->display));
        r->is_disabled = disabled;
    }
    closedir(d);
    return 0;
}

int scan_roms(const char *console_path, bool show_hidden,
              rom_file **out, int *count)
{
    rom_file *arr = NULL;
    int n = 0, cap = 0;

    scan_roms_internal(console_path, show_hidden, &arr, &n, &cap);

    if (n > 1) qsort(arr, n, sizeof(rom_file), cmp_rom_file);

    ap_log("scan_roms: dir=%s show_hidden=%d roms=%d", console_path,
           show_hidden, n);
    *out = arr;
    *count = n;
    return 0;
}

int scan_tools(bool show_hidden, tool_pak **out, int *count)
{
    char tools_dir[SC_MAX_PATH];
    get_tools_path(tools_dir, sizeof(tools_dir));

    DIR *d = opendir(tools_dir);
    if (!d) { *out = NULL; *count = 0; return -1; }

    tool_pak *arr = NULL;
    int n = 0, cap = 0;
    struct dirent *ent;

    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        const char *name = ent->d_name;

        /* Must be a directory. */
        char full[SC_MAX_PATH * 2];
        snprintf(full, sizeof(full), "%s/%s", tools_dir, name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;

        if (!show_hidden && is_hidden(name)) continue;
        if (show_hidden && name[0] == '.') continue;

        bool disabled = ends_with(name, ".pak.disabled");
        if (!ends_with(name, ".pak") && !disabled) continue;

        char base_name[SC_MAX_NAME];
        snprintf(base_name, sizeof(base_name), "%s", name);
        if (disabled) {
            /* Strip .pak.disabled -> base name. */
            char *dot = strstr(base_name, ".pak.disabled");
            if (dot) *dot = '\0';
        } else {
            /* Strip .pak -> base name. */
            char *dot = strstr(base_name, ".pak");
            if (dot) *dot = '\0';
        }

        arr = grow_array(arr, &cap, n, sizeof(tool_pak));
        if (!arr) { closedir(d); *out = NULL; *count = 0; return -1; }

        tool_pak *t = &arr[n++];
        memset(t, 0, sizeof(*t));
        snprintf(t->name, sizeof(t->name), "%s", base_name);
        snprintf(t->path, sizeof(t->path), "%s", full);
        if (disabled) {
            /* Ensure "  [disabled]" suffix fits in the display field. */
            const size_t max_base = sizeof(t->display) - sizeof("  [disabled]");
            if (strlen(base_name) >= max_base)
                base_name[max_base - 1] = '\0';
            snprintf(t->display, sizeof(t->display), "%s  [disabled]",
                     base_name);
        } else
            snprintf(t->display, sizeof(t->display), "%s", base_name);
    }
    closedir(d);

    if (n > 1) qsort(arr, n, sizeof(tool_pak), cmp_tool_pak);

    ap_log("scan_tools: dir=%s tools=%d", tools_dir, n);
    *out = arr;
    *count = n;
    return 0;
}

int scan_shortcuts(shortcut_entry **out, int *count)
{
    char roms_dir[SC_MAX_PATH];
    get_roms_path(roms_dir, sizeof(roms_dir));

    DIR *d = opendir(roms_dir);
    if (!d) { *out = NULL; *count = 0; return -1; }

    shortcut_entry *arr = NULL;
    int n = 0, cap = 0;
    struct dirent *ent;

    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char full[SC_MAX_PATH * 2];
        snprintf(full, sizeof(full), "%s/%s", roms_dir, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        if (!is_shortcut_folder(full))
            continue;

        const char *name = ent->d_name;
        char tag[SC_MAX_TAG];
        extract_tag(name, tag, sizeof(tag));
        bool is_tool = (strcmp(tag, BRIDGE_EMU_TAG) == 0);

        /* Read display name from marker, fall back to folder name. */
        char display[SC_MAX_DISPLAY];
        char *marker = read_shortcut_marker(full);
        if (marker) {
            snprintf(display, sizeof(display), "%s", marker);
            free(marker);
        } else {
            extract_display_name(name, display, sizeof(display));
            /* Strip ZWS or legacy prefix. */
            if (starts_with(display, SHORTCUT_PREFIX)) {
                memmove(display, display + SHORTCUT_PREFIX_LEN,
                        strlen(display + SHORTCUT_PREFIX_LEN) + 1);
            } else if (starts_with(display, LEGACY_PREFIX)) {
                memmove(display, display + LEGACY_PREFIX_LEN,
                        strlen(display + LEGACY_PREFIX_LEN) + 1);
            }
        }

        arr = grow_array(arr, &cap, n, sizeof(shortcut_entry));
        if (!arr) { closedir(d); *out = NULL; *count = 0; return -1; }

        shortcut_entry *sc = &arr[n++];
        memset(sc, 0, sizeof(*sc));
        snprintf(sc->name, sizeof(sc->name), "%s", name);
        snprintf(sc->tag, sizeof(sc->tag), "%s", tag);
        snprintf(sc->display, sizeof(sc->display), "%s", display);
        snprintf(sc->path, sizeof(sc->path), "%s", full);
        sc->is_tool = is_tool;

        /* Resolve target. */
        if (is_tool) {
            char target_file[SC_MAX_PATH * 2];
            snprintf(target_file, sizeof(target_file), "%s/target", full);
            char *data = read_text_file(target_file);
            if (data) {
                /* Trim trailing whitespace. */
                int len = (int)strlen(data);
                while (len > 0 && (data[len-1] == '\n' || data[len-1] == '\r' ||
                                    data[len-1] == ' '))
                    data[--len] = '\0';
                snprintf(sc->target_path, sizeof(sc->target_path), "%s", data);
                free(data);
            }
        } else {
            char m3u_file[SC_MAX_PATH * 2];
            snprintf(m3u_file, sizeof(m3u_file), "%s/%s.m3u", full, name);
            char *data = read_text_file(m3u_file);
            if (data) {
                int len = (int)strlen(data);
                while (len > 0 && (data[len-1] == '\n' || data[len-1] == '\r' ||
                                    data[len-1] == ' '))
                    data[--len] = '\0';
                /* Resolve and normalize the relative path from the shortcut folder. */
                char joined[SC_MAX_PATH * 2];
                snprintf(joined, sizeof(joined), "%s/%s", full, data);
                normalize_path(joined, sc->target_path,
                               sizeof(sc->target_path));
                free(data);
            }
        }
    }
    closedir(d);

    if (n > 1) qsort(arr, n, sizeof(shortcut_entry), cmp_shortcut_entry);

    ap_log("scan_shortcuts: dir=%s shortcuts=%d", roms_dir, n);
    *out = arr;
    *count = n;
    return 0;
}

/* ── Shortcut creation ────────────────────────────────────────── */

int create_rom_shortcut(const char *display_name, const char *tag,
                        const char *console_dir_name, const rom_file *rom,
                        sc_position pos, const app_settings *settings)
{
    char roms_dir[SC_MAX_PATH];
    get_roms_path(roms_dir, sizeof(roms_dir));

    char folder_name[SC_MAX_NAME];
    build_folder_name(pos, display_name, tag, folder_name, sizeof(folder_name));

    char folder_path[SC_MAX_PATH * 2];
    snprintf(folder_path, sizeof(folder_path), "%s/%s", roms_dir, folder_name);

    ap_log("create_rom_shortcut: name=%s tag=%s rom=%s pos=%d multi=%d",
           display_name, tag, rom->name, pos, rom->is_multi_disc);

    if (ensure_dir_exists(folder_path) != 0)
        return -1;

    /*
     * Compute relative path from shortcut folder to ROM.
     * rom->path starts with roms_dir, so the relative portion is
     * rom->path + strlen(roms_dir) + 1.
     */
    const char *rel_from_roms = rom->path + strlen(roms_dir) + 1;
    char rel_path[SC_MAX_PATH];
    if (rom->is_multi_disc) {
        snprintf(rel_path, sizeof(rel_path), "../%s/%s.m3u",
                 rel_from_roms, rom->name);
    } else if (rom->is_cue_folder) {
        snprintf(rel_path, sizeof(rel_path), "../%s/%s.cue",
                 rel_from_roms, rom->name);
    } else {
        snprintf(rel_path, sizeof(rel_path), "../%s", rel_from_roms);
    }

    /* Write .m3u file. */
    char m3u_path[SC_MAX_PATH * 2];
    snprintf(m3u_path, sizeof(m3u_path), "%s/%s.m3u", folder_path, folder_name);
    if (write_text_file(m3u_path, rel_path) != 0)
        return -1;

    /* Write .shortcut marker. */
    write_shortcut_marker(folder_path, display_name);

    /* Artwork. */
    if (settings->copy_artwork) {
        /* Source art: <rom_parent_dir>/.media/<rom_display>.png */
        char art_src[SC_MAX_PATH * 2];
        /* Get the parent directory of the ROM. */
        char rom_parent[SC_MAX_PATH];
        snprintf(rom_parent, sizeof(rom_parent), "%s", rom->path);
        char *last_slash = strrchr(rom_parent, '/');
        if (last_slash) *last_slash = '\0';

        snprintf(art_src, sizeof(art_src), "%s/.media/%s.png",
                 rom_parent, rom->display);

        bool use_bg, force_blk;
        artwork_bg_params(settings, &use_bg, &force_blk);
        generate_artwork_bg(art_src, folder_path, use_bg, force_blk);
    }

    ap_log("create_rom_shortcut: created folder=%s", folder_path);
    return 0;
}

int create_tool_shortcut(const char *display_name, const char *pak_path,
                         sc_position pos, const app_settings *settings)
{
    char roms_dir[SC_MAX_PATH], tools_dir[SC_MAX_PATH];
    get_roms_path(roms_dir, sizeof(roms_dir));
    get_tools_path(tools_dir, sizeof(tools_dir));

    char folder_name[SC_MAX_NAME];
    build_folder_name(pos, display_name, BRIDGE_EMU_TAG,
                      folder_name, sizeof(folder_name));

    char folder_path[SC_MAX_PATH * 2];
    snprintf(folder_path, sizeof(folder_path), "%s/%s", roms_dir, folder_name);

    ap_log("create_tool_shortcut: name=%s pak=%s pos=%d",
           display_name, pak_path, pos);

    if (ensure_dir_exists(folder_path) != 0)
        return -1;

    /* Write target file containing the .pak path. */
    char target_path[SC_MAX_PATH * 2];
    snprintf(target_path, sizeof(target_path), "%s/target", folder_path);
    if (write_text_file(target_path, pak_path) != 0)
        return -1;

    /* Write .m3u pointing to "target". */
    char m3u_path[SC_MAX_PATH * 2];
    snprintf(m3u_path, sizeof(m3u_path), "%s/%s.m3u", folder_path, folder_name);
    if (write_text_file(m3u_path, "target") != 0)
        return -1;

    /* Write .shortcut marker. */
    write_shortcut_marker(folder_path, display_name);

    /* Artwork. */
    if (settings->copy_artwork) {
        char art_src[SC_MAX_PATH * 2];
        snprintf(art_src, sizeof(art_src), "%s/.media/%s.png",
                 tools_dir, display_name);
        bool use_bg, force_blk;
        artwork_bg_params(settings, &use_bg, &force_blk);
        generate_artwork_bg(art_src, folder_path, use_bg, force_blk);
    }

    ap_log("create_tool_shortcut: created folder=%s", folder_path);
    return 0;
}

int remove_shortcut(const char *shortcut_path)
{
    ap_log("remove_shortcut: path=%s", shortcut_path);
    return rmdir_recursive(shortcut_path);
}

bool shortcut_exists(const char *display_name, const char *tag)
{
    char roms_dir[SC_MAX_PATH];
    get_roms_path(roms_dir, sizeof(roms_dir));

    sc_position positions[] = { SC_POS_BOTTOM, SC_POS_TOP, SC_POS_ALPHA };
    for (int i = 0; i < 3; i++) {
        char folder_name[SC_MAX_NAME];
        build_folder_name(positions[i], display_name, tag,
                          folder_name, sizeof(folder_name));
        char full[SC_MAX_PATH * 2];
        snprintf(full, sizeof(full), "%s/%s", roms_dir, folder_name);
        struct stat st;
        if (stat(full, &st) == 0) return true;
    }
    return false;
}

/* ── Bridge emulator ──────────────────────────────────────────── */

static const char *bridge_launch_script =
    "#!/bin/sh\n"
    "# SHORTCUT.pak - Bridge emulator for tool shortcuts.\n"
    "TARGET=$(cat \"$1\")\n"
    "if [ -x \"$TARGET/launch.sh\" ]; then\n"
    "    exec \"$TARGET/launch.sh\"\n"
    "fi\n";

void ensure_bridge_emu(void)
{
#if defined(PLATFORM_MAC)
    return; /* Not needed on macOS. */
#else
    char emus_dir[SC_MAX_PATH];
    get_emus_path(emus_dir, sizeof(emus_dir));

    char pak_dir[SC_MAX_PATH * 2];
    snprintf(pak_dir, sizeof(pak_dir), "%s/SHORTCUT.pak", emus_dir);

    char launch_path[SC_MAX_PATH * 2];
    snprintf(launch_path, sizeof(launch_path), "%s/launch.sh", pak_dir);

    struct stat st;
    if (stat(launch_path, &st) == 0) {
        ap_log("ensure_bridge_emu: already present at %s", launch_path);
        return;
    }

    if (ensure_dir_exists(pak_dir) != 0) {
        ap_log("ensure_bridge_emu: failed to create dir %s", pak_dir);
        return;
    }

    FILE *f = fopen(launch_path, "w");
    if (!f) {
        ap_log("ensure_bridge_emu: failed to write %s", launch_path);
        return;
    }
    fputs(bridge_launch_script, f);
    fclose(f);
    chmod(launch_path, 0755);

    ap_log("ensure_bridge_emu: created at %s", launch_path);
#endif
}
