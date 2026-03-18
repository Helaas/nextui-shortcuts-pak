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

static void copy_cstr_trunc(char *out, size_t out_size, const char *src)
{
    size_t len = 0;

    if (!out || out_size == 0) return;
    if (src) len = strlen(src);
    if (len >= out_size) len = out_size - 1;

    if (len > 0 && src)
        memcpy(out, src, len);
    out[len] = '\0';
}

static bool copy_cstr_exact(char *out, size_t out_size, const char *src)
{
    size_t len = 0;

    if (!out || out_size == 0) return false;
    if (src) len = strlen(src);
    if (len >= out_size) {
        out[0] = '\0';
        return false;
    }

    if (len > 0 && src)
        memcpy(out, src, len);
    out[len] = '\0';
    return true;
}

static bool append_cstr_exact(char *out, size_t out_size, const char *suffix)
{
    size_t out_len;
    size_t suffix_len = 0;

    if (!out || out_size == 0) return false;
    if (suffix) suffix_len = strlen(suffix);

    out_len = strlen(out);
    if (out_len + suffix_len >= out_size)
        return false;

    if (suffix_len > 0 && suffix)
        memcpy(out + out_len, suffix, suffix_len);
    out[out_len + suffix_len] = '\0';
    return true;
}

static bool join_path(char *out, size_t out_size,
                      const char *left, const char *right)
{
    size_t left_len;
    size_t right_len;
    size_t pos = 0;
    bool add_sep;
    bool trim_right_sep;

    if (!out || out_size == 0) return false;
    out[0] = '\0';

    if (!left) left = "";
    if (!right) right = "";

    left_len = strlen(left);
    right_len = strlen(right);
    add_sep = (left_len > 0 && right_len > 0 &&
               left[left_len - 1] != '/' && right[0] != '/');
    trim_right_sep = (left_len > 0 && right_len > 0 &&
                      left[left_len - 1] == '/' && right[0] == '/');

    if (trim_right_sep) {
        right++;
        right_len--;
    }

    if (left_len + (add_sep ? 1 : 0) + right_len >= out_size)
        return false;

    if (left_len > 0) {
        memcpy(out, left, left_len);
        pos = left_len;
    }
    if (add_sep)
        out[pos++] = '/';
    if (right_len > 0) {
        memcpy(out + pos, right, right_len);
        pos += right_len;
    }
    out[pos] = '\0';
    return true;
}

static bool join_path_with_suffix(char *out, size_t out_size,
                                  const char *dir, const char *leaf,
                                  const char *suffix)
{
    if (!join_path(out, out_size, dir, leaf))
        return false;
    return append_cstr_exact(out, out_size, suffix);
}

static void copy_with_suffix_trunc(char *out, size_t out_size,
                                   const char *src, const char *suffix)
{
    size_t suffix_len = suffix ? strlen(suffix) : 0;
    size_t src_len = src ? strlen(src) : 0;

    if (!out || out_size == 0) return;
    if (suffix_len >= out_size) {
        out[0] = '\0';
        return;
    }

    if (src_len > out_size - suffix_len - 1)
        src_len = out_size - suffix_len - 1;

    if (src_len > 0 && src)
        memcpy(out, src, src_len);
    if (suffix_len > 0 && suffix)
        memcpy(out + src_len, suffix, suffix_len);
    out[src_len + suffix_len] = '\0';
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
        copy_cstr_trunc(out, (size_t)out_size, name);
        return;
    }
    int len = (int)(open - name);
    /* Trim trailing whitespace. */
    while (len > 0 && name[len - 1] == ' ') len--;
    if (len <= 0) {
        copy_cstr_trunc(out, (size_t)out_size, name);
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
    copy_cstr_trunc(out, (size_t)out_size, name);
}

/* build_folder_name: construct prefixed shortcut folder name.
 * Returns false if the result was truncated. */
bool build_folder_name(sc_position pos, const char *display, const char *tag,
                       char *out, int out_size)
{
    int n;
    switch (pos) {
    case SC_POS_TOP:
        n = snprintf(out, out_size, TOP_PREFIX "%s (%s)", display, tag);
        break;
    case SC_POS_ALPHA:
        n = snprintf(out, out_size, "%s (%s)", display, tag);
        break;
    default: /* SC_POS_BOTTOM */
        n = snprintf(out, out_size, SHORTCUT_PREFIX "%s (%s)", display, tag);
        break;
    }
    return n >= 0 && n < out_size;
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

static bool is_always_hidden_system_entry(const char *name)
{
    if (!name || name[0] == '\0') return false;

    return strcmp(name, "map.txt") == 0 ||
           strcmp(name, ".media") == 0 ||
           strcmp(name, ".DS_Store") == 0 ||
           strcmp(name, ".Spotlight-V100") == 0 ||
           strcmp(name, ".Trashes") == 0 ||
           strcmp(name, ".fseventsd") == 0 ||
           strcmp(name, ".TemporaryItems") == 0 ||
           starts_with(name, "._");
}

/* Extensions that are never ROM files — artwork, metadata, save data. */
static bool is_non_game_extension(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) return false;
    const char *ext = dot + 1;

    /* Images */
    if (strcasecmp(ext, "png") == 0 || strcasecmp(ext, "jpg") == 0 ||
        strcasecmp(ext, "jpeg") == 0 || strcasecmp(ext, "bmp") == 0 ||
        strcasecmp(ext, "gif") == 0 || strcasecmp(ext, "svg") == 0 ||
        strcasecmp(ext, "ico") == 0 || strcasecmp(ext, "webp") == 0)
        return true;

    /* Text / metadata */
    if (strcasecmp(ext, "txt") == 0 || strcasecmp(ext, "xml") == 0 ||
        strcasecmp(ext, "nfo") == 0 || strcasecmp(ext, "htm") == 0 ||
        strcasecmp(ext, "html") == 0 || strcasecmp(ext, "log") == 0 ||
        strcasecmp(ext, "cfg") == 0 || strcasecmp(ext, "ini") == 0 ||
        strcasecmp(ext, "pdf") == 0 || strcasecmp(ext, "doc") == 0 ||
        strcasecmp(ext, "docx") == 0 || strcasecmp(ext, "rtf") == 0)
        return true;

    /* Save data */
    if (strcasecmp(ext, "srm") == 0 || strcasecmp(ext, "sav") == 0 ||
        strcasecmp(ext, "oops") == 0 || strcasecmp(ext, "db") == 0)
        return true;

    /* Preview videos / scrape sidecars */
    if (strcasecmp(ext, "mp4") == 0 || strcasecmp(ext, "m4v") == 0 ||
        strcasecmp(ext, "mkv") == 0 || strcasecmp(ext, "avi") == 0 ||
        strcasecmp(ext, "mov") == 0 || strcasecmp(ext, "webm") == 0)
        return true;

    return false;
}

static bool should_skip_rom_entry(const char *name, bool show_hidden)
{
    if (!name || name[0] == '\0') return true;
    if (!show_hidden && is_hidden(name)) return true;
    if (show_hidden && is_always_hidden_system_entry(name)) return true;
    return false;
}

static bool should_skip_console_dir(const char *name, bool show_hidden)
{
    if (!name || name[0] == '\0') return true;
    if (!show_hidden) return is_hidden(name);
    return is_mac_dotfile(name) || is_always_hidden_system_entry(name);
}

static bool copy_entry_base_name(const char *name, char *out, size_t out_size)
{
    static const char disabled_suffix[] = ".disabled";
    size_t len;
    bool disabled;

    copy_cstr_trunc(out, out_size, name);
    len = strlen(out);
    disabled = ends_with(out, disabled_suffix);
    if (disabled && len >= sizeof(disabled_suffix) - 1)
        out[len - (sizeof(disabled_suffix) - 1)] = '\0';
    return disabled;
}

static bool dir_has_named_companion(const char *dir_path,
                                    const char *base_name,
                                    const char *suffix)
{
    char candidate[SC_MAX_PATH * 2];
    struct stat st;

    if (!join_path_with_suffix(candidate, sizeof(candidate),
                               dir_path, base_name, suffix))
        return false;

    return stat(candidate, &st) == 0 && S_ISREG(st.st_mode);
}

static bool is_multi_disc_dir(const char *dir_path, const char *base_name)
{
    return dir_has_named_companion(dir_path, base_name, ".m3u");
}

static bool is_cue_folder_dir(const char *dir_path, const char *base_name)
{
    return dir_has_named_companion(dir_path, base_name, ".cue");
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
    if (!join_path(marker, sizeof(marker), folder_path, SHORTCUT_MARKER))
        return false;
    struct stat st;
    return stat(marker, &st) == 0;
}

static bool dir_has_rom_candidate_content_internal(const char *path,
                                                   bool show_hidden,
                                                   bool fail_on_open_error,
                                                   bool *out_has_candidate)
{
    DIR *d = opendir(path);
    bool has_candidate = false;

    if (!d) {
        if (out_has_candidate) *out_has_candidate = false;
        return !fail_on_open_error;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        char base_name[SC_MAX_NAME];
        char full[SC_MAX_PATH * 2];
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        if (should_skip_rom_entry(name, show_hidden))
            continue;

        copy_entry_base_name(name, base_name, sizeof(base_name));
        if (!join_path(full, sizeof(full), path, name))
            continue;
        if (stat(full, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            bool child_has_candidate = false;

            if (is_multi_disc_dir(full, base_name) ||
                is_cue_folder_dir(full, base_name)) {
                has_candidate = true;
                break;
            }

            if (!dir_has_rom_candidate_content_internal(full, show_hidden,
                                                        false,
                                                        &child_has_candidate)) {
                closedir(d);
                return false;
            }
            if (child_has_candidate) {
                has_candidate = true;
                break;
            }
            continue;
        }

        if (is_non_game_extension(name))
            continue;

        has_candidate = true;
        break;
    }

    closedir(d);
    if (out_has_candidate) *out_has_candidate = has_candidate;
    return true;
}

static bool dir_has_rom_candidate_content(const char *path, bool show_hidden)
{
    bool has_candidate = false;

    if (!dir_has_rom_candidate_content_internal(path, show_hidden,
                                                false, &has_candidate))
        return false;
    return has_candidate;
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
    char *tmp = path ? strdup(path) : NULL;
    if (!tmp) return -1;

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    int rc = (mkdir(tmp, 0755) == 0 || errno == EEXIST) ? 0 : -1;
    free(tmp);
    return rc;
}

int rmdir_recursive(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char child[SC_MAX_PATH * 2];
        if (!join_path(child, sizeof(child), path, ent->d_name)) {
            closedir(d);
            return -1;
        }
        struct stat st;
        if (lstat(child, &st) != 0) {
            closedir(d);
            return -1;
        }
        if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) {
            if (unlink(child) != 0) {
                closedir(d);
                return -1;
            }
        } else if (rmdir_recursive(child) != 0) {
            closedir(d);
            return -1;
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
    if (cJSON_IsNumber(am)) {
        int mode = am->valueint;
        if (mode >= ART_MODE_BLACK && mode <= ART_MODE_FALLBACK)
            s.artwork_mode = (art_mode)mode;
    }

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
    if (!join_path(marker, sizeof(marker), folder_path, SHORTCUT_MARKER))
        return NULL;
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
    if (!join_path(marker, sizeof(marker), folder_path, SHORTCUT_MARKER))
        return -1;
    return write_text_file(marker, display_name);
}

static bool normalize_path(const char *path, char *out, int out_size)
{
    char *tmp = NULL;
    enum { MAX_PARTS = SC_MAX_PATH / 2 };
    char *parts[MAX_PARTS];
    int count = 0;
    bool absolute;

    if (!path || !out || out_size <= 0) return false;

    tmp = strdup(path);
    if (!tmp) {
        out[0] = '\0';
        return false;
    }

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
                if (count >= MAX_PARTS) { free(tmp); out[0] = '\0'; return false; }
                parts[count++] = token;
            }
            continue;
        }

        if (count >= MAX_PARTS) { free(tmp); out[0] = '\0'; return false; }
        parts[count++] = token;
    }

    if (absolute && !copy_cstr_exact(out, (size_t)out_size, "/")) {
        free(tmp);
        return false;
    }

    for (int i = 0; i < count; i++) {
        size_t len = strlen(out);
        if (len > 0 && out[len - 1] != '/' &&
            !append_cstr_exact(out, (size_t)out_size, "/")) {
            out[0] = '\0';
            free(tmp);
            return false;
        }
        if (!append_cstr_exact(out, (size_t)out_size, parts[i])) {
            out[0] = '\0';
            free(tmp);
            return false;
        }
    }

    if (out[0] == '\0' &&
        !copy_cstr_exact(out, (size_t)out_size, absolute ? "/" : ".")) {
        free(tmp);
        return false;
    }

    free(tmp);
    return true;
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
    void *new_arr = realloc(arr, (size_t)new_cap * elem_size);
    if (!new_arr) return arr; /* keep old allocation so caller can free */
    *cap = new_cap;
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
        if (!join_path(full, sizeof(full), roms_dir, ent->d_name))
            continue;
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;

        if (is_shortcut_folder(full))
            continue;

        const char *name = ent->d_name;

        if (should_skip_console_dir(name, show_hidden)) continue;
        if (!dir_has_rom_candidate_content(full, show_hidden)) continue;

        /* Strip .disabled suffix for tag/display extraction. */
        char base_name[SC_MAX_NAME];
        bool disabled = copy_entry_base_name(name, base_name, sizeof(base_name));

        char tag[SC_MAX_TAG];
        extract_tag(base_name, tag, sizeof(tag));
        if (tag[0] == '\0') continue; /* no emu tag — skip */

        arr = grow_array(arr, &cap, n, sizeof(console_dir));
        if (n >= cap) { closedir(d); free(arr); *out = NULL; *count = 0; return -1; }

        console_dir *c = &arr[n];
        memset(c, 0, sizeof(*c));
        copy_cstr_trunc(c->name, sizeof(c->name), name);
        copy_cstr_trunc(c->tag, sizeof(c->tag), tag);
        if (!copy_cstr_exact(c->path, sizeof(c->path), full)) {
            ap_log("scan_console_dirs: skipping overlong path %s", full);
            continue;
        }
        extract_display_name(base_name, c->display, sizeof(c->display));
        c->is_disabled = disabled;
        n++;
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
                              rom_file **arr, int *n, int *cap,
                              bool fail_on_open_error)
{
    DIR *d = opendir(dir_path);
    if (!d) return fail_on_open_error ? -1 : 0;
    struct dirent *ent;

    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        const char *name = ent->d_name;

        bool disabled;
        char base_name[SC_MAX_NAME];
        char full[SC_MAX_PATH * 2];
        struct stat st;

        if (should_skip_rom_entry(name, show_hidden))
            continue;

        disabled = copy_entry_base_name(name, base_name, sizeof(base_name));

        if (!join_path(full, sizeof(full), dir_path, name))
            continue;
        if (stat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (is_multi_disc_dir(full, base_name)) {
                *arr = grow_array(*arr, cap, *n, sizeof(rom_file));
                if (*n >= *cap) { closedir(d); return -1; }
                rom_file *r = &(*arr)[(*n)++];
                memset(r, 0, sizeof(*r));
                snprintf(r->name, sizeof(r->name), "%s", name);
                snprintf(r->path, sizeof(r->path), "%s", full);
                snprintf(r->display, sizeof(r->display), "%s", base_name);
                r->is_multi_disc = true;
                r->is_disabled = disabled;
                continue;
            }

            if (is_cue_folder_dir(full, base_name)) {
                *arr = grow_array(*arr, cap, *n, sizeof(rom_file));
                if (*n >= *cap) { closedir(d); return -1; }
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
            if (scan_roms_internal(full, show_hidden, arr, n, cap, false) != 0) {
                closedir(d);
                return -1;
            }
            continue;
        }

        /* Regular file — skip known non-game extensions. */
        if (is_non_game_extension(name)) continue;
        *arr = grow_array(*arr, cap, *n, sizeof(rom_file));
        if (*n >= *cap) { closedir(d); return -1; }
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

    int rc = scan_roms_internal(console_path, show_hidden, &arr, &n, &cap,
                                true);
    if (rc != 0) {
        free(arr);
        *out = NULL;
        *count = 0;
        return rc;
    }

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
        if (!join_path(full, sizeof(full), tools_dir, name))
            continue;
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;

        if (!show_hidden && is_hidden(name)) continue;
        if (show_hidden && name[0] == '.') continue;

        bool disabled = ends_with(name, ".pak.disabled");
        if (!ends_with(name, ".pak") && !disabled) continue;

        char base_name[SC_MAX_NAME];
        copy_cstr_trunc(base_name, sizeof(base_name), name);
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
        if (n >= cap) { closedir(d); free(arr); *out = NULL; *count = 0; return -1; }

        tool_pak *t = &arr[n];
        memset(t, 0, sizeof(*t));
        copy_cstr_trunc(t->name, sizeof(t->name), base_name);
        if (!copy_cstr_exact(t->path, sizeof(t->path), full)) {
            ap_log("scan_tools: skipping overlong path %s", full);
            continue;
        }
        if (disabled) {
            copy_with_suffix_trunc(t->display, sizeof(t->display),
                                   base_name, "  [disabled]");
        } else {
            copy_cstr_trunc(t->display, sizeof(t->display), base_name);
        }
        n++;
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
        if (!join_path(full, sizeof(full), roms_dir, ent->d_name))
            continue;
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
            copy_cstr_trunc(display, sizeof(display), marker);
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
        if (n >= cap) { closedir(d); free(arr); *out = NULL; *count = 0; return -1; }

        shortcut_entry *sc = &arr[n];
        memset(sc, 0, sizeof(*sc));
        copy_cstr_trunc(sc->name, sizeof(sc->name), name);
        copy_cstr_trunc(sc->tag, sizeof(sc->tag), tag);
        copy_cstr_trunc(sc->display, sizeof(sc->display), display);
        if (!copy_cstr_exact(sc->path, sizeof(sc->path), full)) {
            ap_log("scan_shortcuts: skipping overlong path %s", full);
            continue;
        }
        sc->is_tool = is_tool;

        /* Resolve target. */
        if (is_tool) {
            char target_file[SC_MAX_PATH * 2];
            if (!join_path(target_file, sizeof(target_file), full, "target")) {
                ap_log("scan_shortcuts: target path too long for %s", full);
                n++;
                continue;
            }
            char *data = read_text_file(target_file);
            if (data) {
                /* Trim trailing whitespace. */
                int len = (int)strlen(data);
                while (len > 0 && (data[len-1] == '\n' || data[len-1] == '\r' ||
                                    data[len-1] == ' '))
                    data[--len] = '\0';
                if (!copy_cstr_exact(sc->target_path, sizeof(sc->target_path), data))
                    ap_log("scan_shortcuts: ignoring overlong target for %s", full);
                free(data);
            }
        } else {
            char m3u_file[SC_MAX_PATH * 2];
            if (!join_path_with_suffix(m3u_file, sizeof(m3u_file), full, name, ".m3u")) {
                ap_log("scan_shortcuts: m3u path too long for %s", full);
                n++;
                continue;
            }
            char *data = read_text_file(m3u_file);
            if (data) {
                int len = (int)strlen(data);
                while (len > 0 && (data[len-1] == '\n' || data[len-1] == '\r' ||
                                    data[len-1] == ' '))
                    data[--len] = '\0';
                /* Resolve and normalize the relative path from the shortcut folder. */
                char joined[SC_MAX_PATH * 2];
                if (join_path(joined, sizeof(joined), full, data) &&
                    !normalize_path(joined, sc->target_path,
                                    sizeof(sc->target_path))) {
                    ap_log("scan_shortcuts: ignoring overlong normalized target for %s",
                           full);
                }
                free(data);
            }
        }
        n++;
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
    if (!build_folder_name(pos, display_name, tag, folder_name, sizeof(folder_name)))
        return -1;

    char folder_path[SC_MAX_PATH * 2];
    if (!join_path(folder_path, sizeof(folder_path), roms_dir, folder_name))
        return -1;

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
    if (!join_path_with_suffix(m3u_path, sizeof(m3u_path),
                               folder_path, folder_name, ".m3u"))
        return -1;
    if (write_text_file(m3u_path, rel_path) != 0)
        return -1;

    /* Write .shortcut marker. */
    if (write_shortcut_marker(folder_path, display_name) != 0)
        ap_log("create_rom_shortcut: failed to write shortcut marker");

    /* Artwork. */
    if (settings->copy_artwork) {
        /* Source art: <rom_parent_dir>/.media/<rom_display>.png */
        char art_src[SC_MAX_PATH * 2];
        /* Get the parent directory of the ROM. */
        char rom_parent[SC_MAX_PATH];
        copy_cstr_trunc(rom_parent, sizeof(rom_parent), rom->path);
        char *last_slash = strrchr(rom_parent, '/');
        if (last_slash) *last_slash = '\0';

        if (!join_path(art_src, sizeof(art_src), rom_parent, ".media") ||
            !append_cstr_exact(art_src, sizeof(art_src), "/") ||
            !append_cstr_exact(art_src, sizeof(art_src), rom->display) ||
            !append_cstr_exact(art_src, sizeof(art_src), ".png"))
            return -1;

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
    if (!build_folder_name(pos, display_name, BRIDGE_EMU_TAG,
                           folder_name, sizeof(folder_name)))
        return -1;

    char folder_path[SC_MAX_PATH * 2];
    if (!join_path(folder_path, sizeof(folder_path), roms_dir, folder_name))
        return -1;

    ap_log("create_tool_shortcut: name=%s pak=%s pos=%d",
           display_name, pak_path, pos);

    if (ensure_dir_exists(folder_path) != 0)
        return -1;

    /* Write target file containing the .pak path. */
    char target_path[SC_MAX_PATH * 2];
    if (!join_path(target_path, sizeof(target_path), folder_path, "target"))
        return -1;
    if (write_text_file(target_path, pak_path) != 0)
        return -1;

    /* Write .m3u pointing to "target". */
    char m3u_path[SC_MAX_PATH * 2];
    if (!join_path_with_suffix(m3u_path, sizeof(m3u_path),
                               folder_path, folder_name, ".m3u"))
        return -1;
    if (write_text_file(m3u_path, "target") != 0)
        return -1;

    /* Write .shortcut marker. */
    if (write_shortcut_marker(folder_path, display_name) != 0)
        ap_log("create_tool_shortcut: failed to write shortcut marker");

    /* Artwork. */
    if (settings->copy_artwork) {
        char art_src[SC_MAX_PATH * 2];
        if (!join_path(art_src, sizeof(art_src), tools_dir, ".media") ||
            !append_cstr_exact(art_src, sizeof(art_src), "/") ||
            !append_cstr_exact(art_src, sizeof(art_src), display_name) ||
            !append_cstr_exact(art_src, sizeof(art_src), ".png"))
            return -1;
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

    struct stat st;
    if (lstat(shortcut_path, &st) != 0)
        return -1;
    if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode))
        return unlink(shortcut_path);

    return rmdir_recursive(shortcut_path);
}

bool shortcut_exists(const char *display_name, const char *tag)
{
    char roms_dir[SC_MAX_PATH];
    get_roms_path(roms_dir, sizeof(roms_dir));

    sc_position positions[] = { SC_POS_BOTTOM, SC_POS_TOP, SC_POS_ALPHA };
    for (int i = 0; i < 3; i++) {
        char folder_name[SC_MAX_NAME];
        if (!build_folder_name(positions[i], display_name, tag,
                               folder_name, sizeof(folder_name)))
            continue;
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
    if (!join_path(pak_dir, sizeof(pak_dir), emus_dir, "SHORTCUT.pak")) {
        ap_log("ensure_bridge_emu: pak path too long");
        return;
    }

    char launch_path[SC_MAX_PATH * 2];
    if (!join_path(launch_path, sizeof(launch_path), pak_dir, "launch.sh")) {
        ap_log("ensure_bridge_emu: launch path too long");
        return;
    }

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
    if (chmod(launch_path, 0755) != 0)
        ap_log("ensure_bridge_emu: chmod failed for %s", launch_path);

    ap_log("ensure_bridge_emu: created at %s", launch_path);
#endif
}
