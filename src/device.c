/*
 * device.c — Core logic: string utilities, path resolution, settings,
 *            filesystem scanning, shortcut CRUD, and bridge emu management.
 */
#ifndef TESTING
#include "apostrophe.h"
#endif
#include "shortcuts.h"
#include "cjson/cjson.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

static bool append_char_exact(char *out, size_t out_size, char ch)
{
    char suffix[2] = { ch, '\0' };
    return append_cstr_exact(out, out_size, suffix);
}

static bool strip_trailing_suffix(char *str, const char *suffix)
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

static bool build_tool_shortcut_m3u_content(const char *roms_dir,
                                            const char *pak_path,
                                            char *out, size_t out_size)
{
    const char *roms_suffix = "/Roms";
    size_t roms_suffix_len = strlen(roms_suffix);
    size_t roms_dir_len;
    size_t sd_root_len;
    const char *rel_from_sd;
    int n;

    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (!roms_dir || !pak_path) return false;

    roms_dir_len = strlen(roms_dir);
    if (roms_dir_len <= roms_suffix_len ||
        strcmp(roms_dir + roms_dir_len - roms_suffix_len, roms_suffix) != 0)
        return false;

    sd_root_len = roms_dir_len - roms_suffix_len;
    if (strncmp(pak_path, roms_dir, sd_root_len) != 0 ||
        pak_path[sd_root_len] != '/')
        return false;

    rel_from_sd = pak_path + sd_root_len + 1;
    if (rel_from_sd[0] == '\0')
        return false;

    n = snprintf(out, out_size, "../../%s", rel_from_sd);
    return n >= 0 && (size_t)n < out_size;
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

static bool copy_fmt_exact(char *out, size_t out_size, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (!out || out_size == 0 || !fmt)
        return false;

    va_start(ap, fmt);
    n = vsnprintf(out, out_size, fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= out_size) {
        out[0] = '\0';
        return false;
    }
    return true;
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

static bool format_console_display_name(const char *name,
                                        char *out, size_t out_size)
{
    char tag[SC_MAX_TAG];
    char display[SC_MAX_DISPLAY];

    if (!name || !out || out_size == 0)
        return false;

    extract_tag(name, tag, sizeof(tag));
    if (tag[0] == '\0') {
        copy_cstr_trunc(out, out_size, name);
        return true;
    }

    extract_display_name(name, display, sizeof(display));
    return copy_fmt_exact(out, out_size, "%s (%s)", display, tag);
}

static void strip_shortcut_sort_prefix(char *name)
{
    if (!name) return;

    if (starts_with(name, SHORTCUT_PREFIX)) {
        memmove(name, name + SHORTCUT_PREFIX_LEN,
                strlen(name + SHORTCUT_PREFIX_LEN) + 1);
    } else if (starts_with(name, LEGACY_PREFIX)) {
        memmove(name, name + LEGACY_PREFIX_LEN,
                strlen(name + LEGACY_PREFIX_LEN) + 1);
    }
}

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

static bool append_encoded_shortcut_display(char *out, size_t out_size,
                                            const char *display)
{
    const unsigned char *p = (const unsigned char *)display;

    if (!display) return true;

    for (; *p; p++) {
        if (*p == '%') {
            if (!append_cstr_exact(out, out_size, "%25"))
                return false;
        } else if (*p == '/') {
            if (!append_cstr_exact(out, out_size, "%2F"))
                return false;
        } else if (!append_char_exact(out, out_size, (char)*p)) {
            return false;
        }
    }
    return true;
}

/* Inverse of append_encoded_shortcut_display: only %25 and %2F are encoded,
 * so this decoder is intentionally narrow. */
static bool decode_shortcut_storage_display(const char *encoded,
                                            char *out, size_t out_size)
{
    size_t pos = 0;

    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (!encoded) return true;

    for (size_t i = 0; encoded[i] != '\0'; i++) {
        char decoded = '\0';

        if (encoded[i] == '%' &&
            encoded[i + 1] != '\0' &&
            encoded[i + 2] != '\0') {
            int hi = hex_value(encoded[i + 1]);
            int lo = hex_value(encoded[i + 2]);
            if (hi >= 0 && lo >= 0) {
                int value = (hi << 4) | lo;
                if (value == '%' || value == '/') {
                    decoded = (char)value;
                    i += 2;
                }
            }
        }

        if (decoded == '\0')
            decoded = encoded[i];

        if (pos + 1 >= out_size) {
            out[0] = '\0';
            return false;
        }
        out[pos++] = decoded;
    }

    out[pos] = '\0';
    return true;
}

static bool build_shortcut_storage_name(sc_position pos,
                                        const char *display,
                                        const char *tag,
                                        char *out, size_t out_size)
{
    const char *prefix = "";

    if (!out || out_size == 0 || !tag) return false;
    out[0] = '\0';

    switch (pos) {
    case SC_POS_TOP:    prefix = TOP_PREFIX; break;
    case SC_POS_BOTTOM: prefix = SHORTCUT_PREFIX; break;
    default:            prefix = ""; break;
    }

    return append_cstr_exact(out, out_size, prefix) &&
           append_encoded_shortcut_display(out, out_size, display) &&
           append_cstr_exact(out, out_size, " (") &&
           append_cstr_exact(out, out_size, tag) &&
           append_cstr_exact(out, out_size, ")");
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
           strcmp(name, ".ports") == 0 ||
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

static void *grow_array(void *arr, int *cap, int count, size_t elem_size);

typedef struct {
    char *key;
    char *value;
} rom_title_map_entry;

typedef struct {
    rom_title_map_entry *entries;
    int count;
    int cap;
} rom_title_map;

static void free_rom_title_map(rom_title_map *map)
{
    if (!map) return;

    for (int i = 0; i < map->count; i++) {
        free(map->entries[i].key);
        free(map->entries[i].value);
    }
    free(map->entries);
    map->entries = NULL;
    map->count = 0;
    map->cap = 0;
}

static bool append_rom_title_map_entry(rom_title_map *map,
                                       const char *key,
                                       const char *value)
{
    char *dup_key;
    char *dup_value;

    if (!map || !key || !value || key[0] == '\0' || value[0] == '\0')
        return true;

    map->entries = grow_array(map->entries, &map->cap, map->count,
                              sizeof(*map->entries));
    if (map->count >= map->cap)
        return false;

    dup_key = strdup(key);
    dup_value = strdup(value);
    if (!dup_key || !dup_value) {
        free(dup_key);
        free(dup_value);
        return false;
    }

    map->entries[map->count].key = dup_key;
    map->entries[map->count].value = dup_value;
    map->count++;
    return true;
}

static const char *lookup_rom_title(const rom_title_map *map, const char *key)
{
    if (!map || !key || key[0] == '\0')
        return NULL;

    for (int i = map->count - 1; i >= 0; i--) {
        if (strcmp(map->entries[i].key, key) == 0)
            return map->entries[i].value;
    }
    return NULL;
}

static bool load_rom_title_map(const char *console_path, rom_title_map *out)
{
    char map_path[SC_MAX_PATH * 2];
    char *data;
    char *line;
    bool ok = true;
    bool first_line = true;

    if (!out)
        return false;
    out->entries = NULL;
    out->count = 0;
    out->cap = 0;

    if (!join_path(map_path, sizeof(map_path), console_path, "map.txt"))
        return false;

    data = read_text_file(map_path);
    if (!data)
        return true;

    line = data;
    while (line && *line) {
        char *next = strchr(line, '\n');
        char *tab;
        char *key;
        char *value;

        if (next)
            *next = '\0';
        {
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\r')
                line[len - 1] = '\0';
        }

        key = line;
        if (first_line &&
            (unsigned char)key[0] == 0xEF &&
            (unsigned char)key[1] == 0xBB &&
            (unsigned char)key[2] == 0xBF) {
            key += 3;
        }
        first_line = false;

        tab = strchr(key, '\t');
        if (tab) {
            *tab = '\0';
            value = tab + 1;
            if (key[0] != '\0' && value[0] != '\0' &&
                !append_rom_title_map_entry(out, key, value)) {
                ok = false;
                break;
            }
        }

        line = next ? next + 1 : NULL;
    }

    free(data);
    if (!ok)
        free_rom_title_map(out);
    return ok;
}

static void resolve_rom_display(const rom_title_map *map,
                                const char *lookup_key,
                                const char *fallback,
                                char *out, size_t out_size)
{
    const char *mapped = lookup_rom_title(map, lookup_key);
    copy_cstr_trunc(out, out_size, mapped ? mapped : fallback);
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
        if (lstat(full, &st) != 0)
            continue;
        if (S_ISLNK(st.st_mode))
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
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long len = ftell(f);
    if (len < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
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
    if (fputs(content, f) == EOF) {
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0)
        return -1;
    return 0;
}

int ensure_dir_exists(const char *path)
{
    char *tmp = path ? strdup(path) : NULL;
    struct stat st;
    if (!tmp) return -1;

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0) {
                if (errno != EEXIST ||
                    stat(tmp, &st) != 0 ||
                    !S_ISDIR(st.st_mode)) {
                    *p = '/';
                    free(tmp);
                    return -1;
                }
            }
            *p = '/';
        }
    }
    int rc;
    if (mkdir(tmp, 0755) == 0) {
        rc = 0;
    } else if (errno == EEXIST &&
               stat(tmp, &st) == 0 &&
               S_ISDIR(st.st_mode)) {
        rc = 0;
    } else {
        rc = -1;
    }
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

void get_userdata_path(char *out, int out_size)
{
#if defined(PLATFORM_MAC)
    char cwd[SC_MAX_PATH];
    if (getcwd(cwd, sizeof(cwd)))
        snprintf(out, out_size, "%s/mock_sdcard/.userdata/" PLATFORM_SUBDIR, cwd);
    else
        snprintf(out, out_size, "./mock_sdcard/.userdata/" PLATFORM_SUBDIR);
#else
    const char *userdata = getenv("USERDATA_PATH");
    const char *sd = getenv("SDCARD_PATH");

    if (userdata && userdata[0]) {
        snprintf(out, out_size, "%s", userdata);
        return;
    }

    snprintf(out, out_size, "%s/.userdata/" PLATFORM_SUBDIR,
             sd && sd[0] ? sd : "/mnt/SDCARD");
#endif
}

void get_shared_userdata_path(char *out, int out_size)
{
#if defined(PLATFORM_MAC)
    char cwd[SC_MAX_PATH];
    if (getcwd(cwd, sizeof(cwd)))
        snprintf(out, out_size, "%s/mock_sdcard/.userdata/shared", cwd);
    else
        snprintf(out, out_size, "./mock_sdcard/.userdata/shared");
#else
    const char *shared = getenv("SHARED_USERDATA_PATH");
    const char *sd = getenv("SDCARD_PATH");

    if (shared && shared[0]) {
        snprintf(out, out_size, "%s", shared);
        return;
    }

    snprintf(out, out_size, "%s/.userdata/shared",
             sd && sd[0] ? sd : "/mnt/SDCARD");
#endif
}

void get_logs_path(char *out, int out_size)
{
#if defined(PLATFORM_MAC)
    char cwd[SC_MAX_PATH];
    if (getcwd(cwd, sizeof(cwd)))
        snprintf(out, out_size, "%s/mock_sdcard/.userdata/" PLATFORM_SUBDIR "/logs", cwd);
    else
        snprintf(out, out_size, "./mock_sdcard/.userdata/" PLATFORM_SUBDIR "/logs");
#else
    const char *logs = getenv("LOGS_PATH");

    if (logs && logs[0]) {
        snprintf(out, out_size, "%s", logs);
        return;
    }

    char userdata[SC_MAX_PATH];
    get_userdata_path(userdata, sizeof(userdata));
    snprintf(out, out_size, "%s/logs", userdata);
#endif
}

void get_settings_path(char *out, int out_size)
{
    char shared[SC_MAX_PATH];
    char shortcuts_dir[SC_MAX_PATH];

    get_shared_userdata_path(shared, sizeof(shared));
    if (!join_path(shortcuts_dir, sizeof(shortcuts_dir), shared, "Shortcuts") ||
        !join_path(out, (size_t)out_size, shortcuts_dir, "settings.json")) {
        if (out && out_size > 0)
            out[0] = '\0';
    }
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

/* ── Theme background color ──────────────────────────────────── */

static sc_color hex_to_sc_color(const char *hex)
{
    sc_color c = {0, 0, 0, 255};
    if (!hex || !hex[0]) return c;
    if (hex[0] == '#') hex++;
    else if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex += 2;
    unsigned long val = strtoul(hex, NULL, 16);
    c.r = (uint8_t)((val >> 16) & 0xFF);
    c.g = (uint8_t)((val >>  8) & 0xFF);
    c.b = (uint8_t)( val        & 0xFF);
    return c;
}

sc_color get_theme_bg_color(void)
{
    /* Cached for the process lifetime — regenerating artwork after an
     * in-session theme change requires restarting the app. */
    static sc_color cached = {0, 0, 0, 255};
    static bool loaded = false;
    if (loaded) return cached;
    loaded = true;

#if defined(PLATFORM_MAC)
    return cached; /* No nextval.elf on macOS dev builds. */
#else
    /* Find nextval.elf: prefer SYSTEM_PATH env, fall back to platform path. */
    const char *nextval_path = NULL;
    char nextval_env_buf[256] = {0};
    const char *system_path = getenv("SYSTEM_PATH");
    if (system_path && system_path[0]) {
        snprintf(nextval_env_buf, sizeof(nextval_env_buf),
                 "%s/bin/nextval.elf", system_path);
        if (access(nextval_env_buf, X_OK) == 0)
            nextval_path = nextval_env_buf;
    }
    if (!nextval_path) {
        static const char *fallback =
            "/mnt/SDCARD/.system/" PLATFORM_SUBDIR "/bin/nextval.elf";
        if (access(fallback, X_OK) == 0)
            nextval_path = fallback;
    }
    if (!nextval_path) {
        ap_log("get_theme_bg_color: nextval.elf not found, using black");
        return cached;
    }

    /* Spawn nextval.elf via fork/execv (no shell) so SYSTEM_PATH contents
     * cannot be interpreted as shell metacharacters. */
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        ap_log("get_theme_bg_color: pipe() failed errno=%d", errno);
        return cached;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        ap_log("get_theme_bg_color: fork() failed errno=%d", errno);
        return cached;
    }
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        char *const argv[] = { (char *)nextval_path, NULL };
        execv(nextval_path, argv);
        _exit(127);
    }
    close(pipefd[1]);

    FILE *fp = fdopen(pipefd[0], "r");
    if (!fp) {
        ap_log("get_theme_bg_color: fdopen() failed errno=%d", errno);
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        return cached;
    }

    char json_buf[4096] = {0};
    size_t total = 0;
    while (total < sizeof(json_buf) - 1) {
        size_t n = fread(json_buf + total, 1, sizeof(json_buf) - 1 - total, fp);
        if (n == 0) break;
        total += n;
    }
    json_buf[total] = '\0';
    fclose(fp);
    waitpid(pid, NULL, 0);

    if (total == sizeof(json_buf) - 1)
        ap_log("get_theme_bg_color: nextval output truncated at %zu bytes",
               total);

    cJSON *json = cJSON_Parse(json_buf);
    if (!json) {
        ap_log("get_theme_bg_color: failed to parse nextval output");
        return cached;
    }

    /* Prefer color7 (NextUI ≥ PR#661), fall back to legacy bgcolor. */
    cJSON *color = cJSON_GetObjectItem(json, "color7");
    if (!cJSON_IsString(color) || !color->valuestring[0])
        color = cJSON_GetObjectItem(json, "bgcolor");

    if (cJSON_IsString(color) && color->valuestring[0]) {
        cached = hex_to_sc_color(color->valuestring);
        ap_log("get_theme_bg_color: %s → r=%u g=%u b=%u",
               color->valuestring, cached.r, cached.g, cached.b);
    }

    cJSON_Delete(json);
    return cached;
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
        if (ensure_dir_exists(dir) != 0)
            return -1;
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
                       bool *write_when_missing_art)
{
    switch (s->artwork_mode) {
    case ART_MODE_WALLPAPER:
        *use_global_bg = true;  *write_when_missing_art = true;  break;
    case ART_MODE_FALLBACK:
        *use_global_bg = true;  *write_when_missing_art = false; break;
    default: /* ART_MODE_BLACK */
        *use_global_bg = false; *write_when_missing_art = true;  break;
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
        if (lstat(full, &st) != 0 ||
            S_ISLNK(st.st_mode) ||
            !S_ISDIR(st.st_mode))
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
        if (!format_console_display_name(base_name, c->display,
                                         sizeof(c->display))) {
            ap_log("scan_console_dirs: skipping overlong display %s",
                   base_name);
            continue;
        }
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
                              const rom_title_map *title_map,
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
        if (lstat(full, &st) != 0) continue;
        if (S_ISLNK(st.st_mode)) continue;

        if (S_ISDIR(st.st_mode)) {
            if (is_multi_disc_dir(full, base_name)) {
                *arr = grow_array(*arr, cap, *n, sizeof(rom_file));
                if (*n >= *cap) { closedir(d); return -1; }
                rom_file *r = &(*arr)[*n];
                char lookup_key[SC_MAX_NAME];
                memset(r, 0, sizeof(*r));
                copy_cstr_trunc(r->name, sizeof(r->name), name);
                if (!copy_cstr_exact(r->path, sizeof(r->path), full)) {
                    ap_log("scan_roms: skipping overlong path %s", full);
                    continue;
                }
                copy_cstr_trunc(r->source_stem, sizeof(r->source_stem),
                                base_name);
                if (!copy_fmt_exact(lookup_key, sizeof(lookup_key),
                                    "%s.m3u", base_name)) {
                    closedir(d);
                    return -1;
                }
                resolve_rom_display(title_map, lookup_key, base_name,
                                    r->display, sizeof(r->display));
                r->is_multi_disc = true;
                r->is_disabled = disabled;
                (*n)++;
                continue;
            }

            if (is_cue_folder_dir(full, base_name)) {
                *arr = grow_array(*arr, cap, *n, sizeof(rom_file));
                if (*n >= *cap) { closedir(d); return -1; }
                rom_file *r = &(*arr)[*n];
                char lookup_key[SC_MAX_NAME];
                memset(r, 0, sizeof(*r));
                copy_cstr_trunc(r->name, sizeof(r->name), name);
                if (!copy_cstr_exact(r->path, sizeof(r->path), full)) {
                    ap_log("scan_roms: skipping overlong path %s", full);
                    continue;
                }
                copy_cstr_trunc(r->source_stem, sizeof(r->source_stem),
                                base_name);
                if (!copy_fmt_exact(lookup_key, sizeof(lookup_key),
                                    "%s.cue", base_name)) {
                    closedir(d);
                    return -1;
                }
                resolve_rom_display(title_map, lookup_key, base_name,
                                    r->display, sizeof(r->display));
                r->is_cue_folder = true;
                r->is_disabled = disabled;
                (*n)++;
                continue;
            }

            /* Plain subfolder — recurse. */
            if (scan_roms_internal(full, show_hidden, title_map,
                                   arr, n, cap, false) != 0) {
                closedir(d);
                return -1;
            }
            continue;
        }

        /* Regular file — skip known non-game extensions. */
        if (is_non_game_extension(name)) continue;
        *arr = grow_array(*arr, cap, *n, sizeof(rom_file));
        if (*n >= *cap) { closedir(d); return -1; }
        rom_file *r = &(*arr)[*n];
        memset(r, 0, sizeof(*r));
        copy_cstr_trunc(r->name, sizeof(r->name), name);
        if (!copy_cstr_exact(r->path, sizeof(r->path), full)) {
            ap_log("scan_roms: skipping overlong path %s", full);
            continue;
        }
        strip_extension(base_name, r->source_stem, sizeof(r->source_stem));
        resolve_rom_display(title_map, base_name, r->source_stem,
                            r->display, sizeof(r->display));
        r->is_disabled = disabled;
        (*n)++;
    }
    closedir(d);
    return 0;
}

int scan_roms(const char *console_path, bool show_hidden,
              rom_file **out, int *count)
{
    rom_file *arr = NULL;
    int n = 0, cap = 0;
    rom_title_map title_map = {0};
    int rc;

    if (!load_rom_title_map(console_path, &title_map)) {
        *out = NULL;
        *count = 0;
        return -1;
    }

    rc = scan_roms_internal(console_path, show_hidden, &title_map,
                            &arr, &n, &cap, true);
    free_rom_title_map(&title_map);
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
        if (lstat(full, &st) != 0 ||
            S_ISLNK(st.st_mode) ||
            !S_ISDIR(st.st_mode))
            continue;

        if (!show_hidden && is_hidden(name)) continue;
        if (show_hidden && name[0] == '.') continue;

        bool disabled = ends_with(name, ".pak.disabled");
        if (!ends_with(name, ".pak") && !disabled) continue;

        char base_name[SC_MAX_NAME];
        copy_cstr_trunc(base_name, sizeof(base_name), name);
        if (disabled) {
            /* Strip .pak.disabled -> base name. */
            if (!strip_trailing_suffix(base_name, ".pak.disabled"))
                continue;
        } else {
            /* Strip .pak -> base name. */
            if (!strip_trailing_suffix(base_name, ".pak"))
                continue;
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
        if (lstat(full, &st) != 0 ||
            S_ISLNK(st.st_mode) ||
            !S_ISDIR(st.st_mode))
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
            char decoded[SC_MAX_DISPLAY];
            extract_display_name(name, display, sizeof(display));
            strip_shortcut_sort_prefix(display);
            if (decode_shortcut_storage_display(display, decoded,
                                                sizeof(decoded)))
                copy_cstr_trunc(display, sizeof(display), decoded);
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
        } else if (!resolve_shortcut_target_path(full, name,
                                                 sc->target_path,
                                                 sizeof(sc->target_path))) {
            ap_log("scan_shortcuts: unable to resolve target for %s", full);
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

bool resolve_shortcut_target_path(const char *shortcut_folder_path,
                                  const char *shortcut_name,
                                  char *out, int out_size)
{
    char m3u_file[SC_MAX_PATH * 2];
    char joined[SC_MAX_PATH * 2];
    char *data = NULL;
    bool ok = false;

    if (!out || out_size <= 0) return false;
    out[0] = '\0';

    if (!shortcut_folder_path || !shortcut_name)
        return false;

    if (!join_path_with_suffix(m3u_file, sizeof(m3u_file),
                               shortcut_folder_path, shortcut_name, ".m3u"))
        return false;

    data = read_text_file(m3u_file);
    if (!data) return false;

    {
        int len = (int)strlen(data);
        while (len > 0 && (data[len - 1] == '\n' || data[len - 1] == '\r' ||
                           data[len - 1] == ' ' || data[len - 1] == '\t'))
            data[--len] = '\0';
    }

    if (data[0] == '\0')
        goto cleanup;

    if (data[0] == '/') {
        ok = normalize_path(data, out, out_size);
        goto cleanup;
    }

    if (!join_path(joined, sizeof(joined), shortcut_folder_path, data))
        goto cleanup;

    ok = normalize_path(joined, out, out_size);

cleanup:
    free(data);
    return ok;
}

bool build_rom_target_path(const rom_file *rom, char *out, int out_size)
{
    const char *suffix;
    const char *stem;

    if (!out || out_size <= 0) return false;
    out[0] = '\0';
    if (!rom) return false;

    if (!rom->is_multi_disc && !rom->is_cue_folder)
        return copy_cstr_exact(out, (size_t)out_size, rom->path);

    suffix = rom->is_multi_disc ? ".m3u" : ".cue";
    stem = rom->source_stem[0] != '\0' ? rom->source_stem : rom->display;
    return join_path_with_suffix(out, (size_t)out_size, rom->path,
                                 stem, suffix);
}

bool rom_matches_shortcut_target(const rom_file *rom,
                                 const shortcut_entry *shortcut)
{
    char rom_target[SC_MAX_PATH];

    if (!rom || !shortcut || shortcut->target_path[0] == '\0')
        return false;
    if (!build_rom_target_path(rom, rom_target, sizeof(rom_target)))
        return false;
    return strcmp(rom_target, shortcut->target_path) == 0;
}

static bool build_rom_art_src_path(const rom_file *rom, char *out,
                                   size_t out_size)
{
    char rom_parent[SC_MAX_PATH];
    char art_name[SC_MAX_DISPLAY];
    char *last_slash;

    if (!rom || !out || out_size == 0)
        return false;
    out[0] = '\0';

    copy_cstr_trunc(rom_parent, sizeof(rom_parent), rom->path);
    last_slash = strrchr(rom_parent, '/');
    if (!last_slash)
        return false;
    *last_slash = '\0';

    copy_cstr_trunc(art_name, sizeof(art_name),
                    rom->source_stem[0] != '\0' ? rom->source_stem
                                                : rom->display);

    return join_path(out, out_size, rom_parent, ".media") &&
           append_cstr_exact(out, out_size, "/") &&
           append_cstr_exact(out, out_size, art_name) &&
           append_cstr_exact(out, out_size, ".png");
}

static bool build_root_media_dir(char *out, size_t out_size)
{
    char roms_dir[SC_MAX_PATH];

    get_roms_path(roms_dir, sizeof(roms_dir));
    return join_path(out, out_size, roms_dir, ".media");
}

static bool build_shortcut_thumbnail_path(const char *shortcut_name,
                                          char *out, size_t out_size)
{
    char media_dir[SC_MAX_PATH * 2];

    if (!shortcut_name || shortcut_name[0] == '\0')
        return false;
    return build_root_media_dir(media_dir, sizeof(media_dir)) &&
           join_path_with_suffix(out, out_size, media_dir, shortcut_name,
                                 ".png");
}

static int copy_binary_file(const char *src, const char *dst)
{
    FILE *in = NULL;
    FILE *out = NULL;
    unsigned char buf[8192];
    int rc = -1;

    if (!src || !dst) return -1;
    if (strcmp(src, dst) == 0) return 0;

    in = fopen(src, "rb");
    if (!in)
        goto cleanup;

    out = fopen(dst, "wb");
    if (!out)
        goto cleanup;

    for (;;) {
        size_t n = fread(buf, 1, sizeof(buf), in);
        if (n > 0 && fwrite(buf, 1, n, out) != n)
            goto cleanup;
        if (n == 0)
            break;
        if (ferror(in))
            goto cleanup;
    }
    if (ferror(in))
        goto cleanup;

    if (fclose(out) != 0) {
        out = NULL;
        goto cleanup;
    }
    out = NULL;
    rc = 0;

cleanup:
    if (out) fclose(out);
    if (in) fclose(in);
    return rc;
}

int remove_shortcut_thumbnail(const char *shortcut_name)
{
    char thumb_path[SC_MAX_PATH * 2];

    if (!build_shortcut_thumbnail_path(shortcut_name, thumb_path,
                                       sizeof(thumb_path)))
        return -1;
    if (unlink(thumb_path) == 0 || errno == ENOENT)
        return 0;
    return -1;
}

static int rename_shortcut_thumbnail(const char *old_name,
                                     const char *new_name)
{
    char old_path[SC_MAX_PATH * 2];
    char new_path[SC_MAX_PATH * 2];

    if (!old_name || !new_name || strcmp(old_name, new_name) == 0)
        return 0;
    if (!build_shortcut_thumbnail_path(old_name, old_path,
                                       sizeof(old_path)) ||
        !build_shortcut_thumbnail_path(new_name, new_path,
                                       sizeof(new_path)))
        return -1;
    if (rename(old_path, new_path) == 0 || errno == ENOENT)
        return 0;
    return -1;
}

int sync_shortcut_thumbnail(const char *shortcut_name,
                            const char *art_src_path)
{
    char media_dir[SC_MAX_PATH * 2];
    char thumb_path[SC_MAX_PATH * 2];
    struct stat st;

    if (!build_shortcut_thumbnail_path(shortcut_name, thumb_path,
                                       sizeof(thumb_path)))
        return -1;

    if (!art_src_path)
        return remove_shortcut_thumbnail(shortcut_name);

    if (stat(art_src_path, &st) != 0) {
        if (errno == ENOENT)
            return remove_shortcut_thumbnail(shortcut_name);
        ap_log("sync_shortcut_thumbnail: stat(%s) failed errno=%d",
               art_src_path, errno);
        return -1;
    }

    if (!S_ISREG(st.st_mode))
        return remove_shortcut_thumbnail(shortcut_name);

    if (!build_root_media_dir(media_dir, sizeof(media_dir)) ||
        ensure_dir_exists(media_dir) != 0)
        return -1;

    return copy_binary_file(art_src_path, thumb_path);
}

/* ── Shortcut creation ────────────────────────────────────────── */

int create_rom_shortcut(const char *display_name, const char *tag,
                        const rom_file *rom, sc_position pos,
                        const app_settings *settings)
{
    char roms_dir[SC_MAX_PATH];
    get_roms_path(roms_dir, sizeof(roms_dir));

    char folder_name[SC_MAX_NAME];
    if (!build_shortcut_storage_name(pos, display_name, tag,
                                     folder_name, sizeof(folder_name)))
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
    size_t roms_dir_len = strlen(roms_dir);
    if (strncmp(rom->path, roms_dir, roms_dir_len) != 0 ||
        rom->path[roms_dir_len] != '/') {
        ap_log("create_rom_shortcut: rom path does not start with roms dir");
        return -1;
    }
    const char *rel_from_roms = rom->path + roms_dir_len + 1;
    char rel_path[SC_MAX_PATH];
    int rel_path_len;
    if (rom->is_multi_disc) {
        rel_path_len = snprintf(rel_path, sizeof(rel_path), "../%s/%s.m3u",
                                rel_from_roms, rom->source_stem);
    } else if (rom->is_cue_folder) {
        rel_path_len = snprintf(rel_path, sizeof(rel_path), "../%s/%s.cue",
                                rel_from_roms, rom->source_stem);
    } else {
        rel_path_len = snprintf(rel_path, sizeof(rel_path), "../%s",
                                rel_from_roms);
    }
    if (rel_path_len < 0 || (size_t)rel_path_len >= sizeof(rel_path)) {
        ap_log("create_rom_shortcut: relative path too long");
        return -1;
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
        char art_src[SC_MAX_PATH * 2];
        if (!build_rom_art_src_path(rom, art_src, sizeof(art_src)))
            return -1;

        bool use_bg, write_when_missing_art;
        artwork_bg_params(settings, &use_bg, &write_when_missing_art);
        generate_artwork_bg(art_src, folder_path, use_bg,
                            write_when_missing_art, get_theme_bg_color());
        if (sync_shortcut_thumbnail(folder_name, art_src) != 0)
            ap_log("create_rom_shortcut: failed to sync root thumbnail");
    }

    if (resume_sync_for_shortcut_path(folder_path) != 0)
        ap_log("create_rom_shortcut: resume sync failed for %s", folder_path);

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
    if (!build_shortcut_storage_name(pos, display_name, BRIDGE_EMU_TAG,
                                     folder_name, sizeof(folder_name)))
        return -1;

    char folder_path[SC_MAX_PATH * 2];
    if (!join_path(folder_path, sizeof(folder_path), roms_dir, folder_name))
        return -1;

    char m3u_content[SC_MAX_PATH];
    if (!build_tool_shortcut_m3u_content(roms_dir, pak_path,
                                         m3u_content, sizeof(m3u_content))) {
        ap_log("create_tool_shortcut: unable to build m3u target for %s",
               pak_path);
        return -1;
    }

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

    /* Write .m3u with relative path to the tool's .pak directory,
     * so NextUI's Game Tracker records the real tool name. */
    char m3u_path[SC_MAX_PATH * 2];
    if (!join_path_with_suffix(m3u_path, sizeof(m3u_path),
                               folder_path, folder_name, ".m3u"))
        return -1;
    if (write_text_file(m3u_path, m3u_content) != 0)
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
        bool use_bg, write_when_missing_art;
        artwork_bg_params(settings, &use_bg, &write_when_missing_art);
        generate_artwork_bg(art_src, folder_path, use_bg,
                            write_when_missing_art, get_theme_bg_color());
        if (sync_shortcut_thumbnail(folder_name, art_src) != 0)
            ap_log("create_tool_shortcut: failed to sync root thumbnail");
    }

    ap_log("create_tool_shortcut: created folder=%s", folder_path);
    return 0;
}

int remove_shortcut(const char *shortcut_path)
{
    ap_log("remove_shortcut: path=%s", shortcut_path);

    if (resume_remove_alias_for_shortcut_path(shortcut_path) != 0)
        ap_log("remove_shortcut: resume alias cleanup failed for %s",
               shortcut_path);

    struct stat st;
    if (lstat(shortcut_path, &st) != 0)
        return -1;
    const char *shortcut_name = strrchr(shortcut_path, '/');
    shortcut_name = shortcut_name ? shortcut_name + 1 : shortcut_path;
    /* Remove the root thumbnail first; if the folder delete below fails,
     * the next regenerate will rebuild the thumbnail. */
    if (remove_shortcut_thumbnail(shortcut_name) != 0)
        ap_log("remove_shortcut: failed to remove root thumbnail");
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
        if (!build_shortcut_storage_name(positions[i], display_name, tag,
                                         folder_name, sizeof(folder_name)))
            continue;
        char full[SC_MAX_PATH * 2];
        if (!join_path(full, sizeof(full), roms_dir, folder_name))
            continue;
        struct stat st;
        if (stat(full, &st) == 0) return true;
    }
    return false;
}

/* ── Shortcut rename ──────────────────────────────────────────── */

static sc_position detect_position(const char *folder_name)
{
    if (starts_with(folder_name, SHORTCUT_PREFIX)) return SC_POS_BOTTOM;
    if (starts_with(folder_name, TOP_PREFIX))      return SC_POS_TOP;
    if (starts_with(folder_name, LEGACY_PREFIX))   return SC_POS_BOTTOM;
    return SC_POS_ALPHA;
}

int rename_shortcut(const shortcut_entry *sc, const char *new_display)
{
    ap_log("rename_shortcut: old=%s new=%s", sc->display, new_display);

    sc_position pos = detect_position(sc->name);

    char new_folder_name[SC_MAX_NAME];
    if (!build_shortcut_storage_name(pos, new_display, sc->tag,
                                     new_folder_name, sizeof(new_folder_name)))
        return -1;

    char roms_dir[SC_MAX_PATH];
    get_roms_path(roms_dir, sizeof(roms_dir));

    char new_folder_path[SC_MAX_PATH * 2];
    if (!join_path(new_folder_path, sizeof(new_folder_path),
                   roms_dir, new_folder_name))
        return -1;

    /* Check for collision. */
    struct stat st;
    if (stat(new_folder_path, &st) == 0) return -1;

    /* Rename the folder first to avoid partial updates. */
    if (rename(sc->path, new_folder_path) != 0) {
        ap_log("rename_shortcut: folder rename failed: %s", strerror(errno));
        return -1;
    }

    /* Rename .m3u file inside the (now renamed) folder. */
    char old_m3u[SC_MAX_PATH * 2], new_m3u[SC_MAX_PATH * 2];
    if (!join_path_with_suffix(old_m3u, sizeof(old_m3u),
                               new_folder_path, sc->name, ".m3u") ||
        !join_path_with_suffix(new_m3u, sizeof(new_m3u),
                               new_folder_path, new_folder_name, ".m3u")) {
        ap_log("rename_shortcut: m3u path too long");
        rename(new_folder_path, sc->path);
        return -1;
    }
    if (rename(old_m3u, new_m3u) != 0) {
        ap_log("rename_shortcut: m3u rename failed: %s", strerror(errno));
        rename(new_folder_path, sc->path);
        return -1;
    }

    /* Update .shortcut marker with new display name. */
    if (write_shortcut_marker(new_folder_path, new_display) != 0) {
        ap_log("rename_shortcut: marker update failed");
        rename(new_m3u, old_m3u);
        rename(new_folder_path, sc->path);
        return -1;
    }

    if (rename_shortcut_thumbnail(sc->name, new_folder_name) != 0)
        ap_log("rename_shortcut: thumbnail rename failed");

    if (resume_remove_alias_for_shortcut_path(sc->path) != 0)
        ap_log("rename_shortcut: old resume alias cleanup failed for %s",
               sc->path);
    if (resume_sync_for_shortcut_path(new_folder_path) != 0)
        ap_log("rename_shortcut: new resume alias sync failed for %s",
               new_folder_path);

    ap_log("rename_shortcut: done -> %s", new_folder_path);
    return 0;
}

/* ── Bridge emulator ──────────────────────────────────────────── */

static const char *bridge_launch_script =
    "#!/bin/sh\n"
    "# SHORTCUT.pak - Bridge emulator for tool shortcuts.\n"
    "# Old format: $1 is the target file containing the real tool path.\n"
    "# New format: $1 is the tool's .pak directory path.\n"
    "if [ -f \"$1\" ]; then\n"
    "    TARGET=$(cat \"$1\")\n"
    "elif [ -d \"$1\" ]; then\n"
    "    TARGET=\"$1\"\n"
    "else\n"
    "    exit 1\n"
    "fi\n"
    "if [ -x \"$TARGET/launch.sh\" ]; then\n"
    "    exec \"$TARGET/launch.sh\"\n"
    "fi\n";

#ifdef TESTING
const char *bridge_launch_script_for_tests(void)
{
    return bridge_launch_script;
}
#endif

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

    if (ensure_dir_exists(pak_dir) != 0) {
        ap_log("ensure_bridge_emu: failed to create dir %s", pak_dir);
        return;
    }

    FILE *f = fopen(launch_path, "w");
    if (!f) {
        ap_log("ensure_bridge_emu: failed to write %s", launch_path);
        return;
    }
    if (fputs(bridge_launch_script, f) == EOF) {
        ap_log("ensure_bridge_emu: fputs failed for %s", launch_path);
        fclose(f);
        return;
    }
    if (fclose(f) != 0) {
        ap_log("ensure_bridge_emu: fclose failed for %s", launch_path);
        return;
    }
    if (chmod(launch_path, 0755) != 0)
        ap_log("ensure_bridge_emu: chmod failed for %s", launch_path);

    ap_log("ensure_bridge_emu: created at %s", launch_path);
#endif
}
