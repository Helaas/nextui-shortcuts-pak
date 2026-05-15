/*
 * resume_sync.c — Hook-based resume alias syncing for ROM shortcuts.
 *
 * NextUI checks resume state using the shortcut folder's own .m3u filename.
 * MinArch writes resume metadata using the canonical launched target key.
 * This module mirrors the canonical slot file onto the shortcut key after a
 * ROM exits, so shortcut folders can show "X Resume" without a background
 * daemon.
 */
#include "shortcuts.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char shortcut_path[SC_MAX_PATH];
    char alias_slot_path[SC_MAX_PATH];
} manifest_entry;

static const char resume_hook_script[] =
    "#!/bin/sh\n"
    "\n"
    "if [ \"${HOOK_PHASE:-}\" != \"post\" ]; then\n"
    "    exit 0\n"
    "fi\n"
    "if [ \"${HOOK_TYPE:-}\" != \"rom\" ] && [ \"${HOOK_TYPE:-}\" != \"tool\" ]; then\n"
    "    exit 0\n"
    "fi\n"
    "\n"
    "HELPER=\"${SDCARD_PATH:-/mnt/SDCARD}/Tools/${PLATFORM:-tg5040}/Shortcuts.pak/shortcuts\"\n"
    "[ -x \"$HELPER\" ] || exit 0\n"
    "\n"
    "LD_LIBRARY_PATH=\"${LD_LIBRARY_PATH:-${SDCARD_PATH:-/mnt/SDCARD}/.system/${PLATFORM:-tg5040}/lib:/usr/miyoo/lib:/usr/miyoo/lib:}\"\n"
    "export LD_LIBRARY_PATH\n"
    "\n"
    "LOG_DIR=\"${LOGS_PATH:-${USERDATA_PATH:-${SDCARD_PATH:-/mnt/SDCARD}/.userdata/${PLATFORM:-tg5040}}/logs}\"\n"
    "mkdir -p \"$LOG_DIR\"\n"
    "cd \"${SDCARD_PATH:-/mnt/SDCARD}/Tools/${PLATFORM:-tg5040}/Shortcuts.pak\" || exit 0\n"
    "\"./shortcuts\" --resume-sync-hook >>\"$LOG_DIR/shortcuts-resume-sync.txt\" 2>&1\n";

static void copy_cstr_trunc_local(char *out, size_t out_size, const char *src)
{
    size_t len = 0;

    if (!out || out_size == 0) return;
    if (src) len = strlen(src);
    if (len >= out_size) len = out_size - 1;

    if (len > 0 && src)
        memcpy(out, src, len);
    out[len] = '\0';
}

static const char *path_basename_local(const char *path)
{
    const char *base;

    if (!path) return "";
    base = strrchr(path, '/');
    return base ? base + 1 : path;
}

static bool path_exists_local(const char *path)
{
    struct stat st;

    return path && lstat(path, &st) == 0;
}

static bool ensure_parent_dir_local(const char *path)
{
    char parent[SC_MAX_PATH];
    char *slash;

    if (!path) return false;
    copy_cstr_trunc_local(parent, sizeof(parent), path);
    slash = strrchr(parent, '/');
    if (!slash) return true;
    *slash = '\0';
    if (parent[0] == '\0') return true;
    return ensure_dir_exists(parent) == 0;
}

static void trim_trailing_whitespace_local(char *text)
{
    int len;

    if (!text) return;
    len = (int)strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r' ||
                       text[len - 1] == ' ' || text[len - 1] == '\t'))
        text[--len] = '\0';
}

static bool remove_file_if_exists_local(const char *path)
{
    if (!path || path[0] == '\0') return true;
    if (unlink(path) == 0) return true;
    return errno == ENOENT;
}

static bool write_executable_file_local(const char *path, const char *content)
{
    FILE *f;

    if (!ensure_parent_dir_local(path))
        return false;

    f = fopen(path, "w");
    if (!f) return false;

    if (fputs(content, f) == EOF) {
        fclose(f);
        return false;
    }
    if (fclose(f) != 0)
        return false;

    return chmod(path, 0755) == 0;
}

static bool build_resume_hook_path(char *out, size_t out_size)
{
    char userdata[SC_MAX_PATH];

    get_userdata_path(userdata, sizeof(userdata));
    return snprintf(out, out_size, "%s/.hooks/post-launch.d/shortcuts-resume.sh",
                    userdata) < (int)out_size;
}

static bool build_manifest_path(char *out, size_t out_size)
{
    char shared[SC_MAX_PATH];

    get_shared_userdata_path(shared, sizeof(shared));
    return snprintf(out, out_size, "%s/Shortcuts/resume_aliases.tsv", shared) <
           (int)out_size;
}

static bool parse_shortcut_name_tag_from_path(const char *shortcut_path,
                                              char *name, size_t name_size,
                                              char *tag, size_t tag_size)
{
    const char *base = path_basename_local(shortcut_path);

    if (!shortcut_path || base[0] == '\0')
        return false;

    copy_cstr_trunc_local(name, name_size, base);
    extract_tag(base, tag, (int)tag_size);
    return name[0] != '\0' && tag[0] != '\0';
}

static bool is_rom_shortcut_path_local(const char *shortcut_path,
                                       char *name, size_t name_size,
                                       char *tag, size_t tag_size)
{
    if (!shortcut_path || !path_exists_local(shortcut_path) ||
        !is_shortcut_folder(shortcut_path))
        return false;

    if (!parse_shortcut_name_tag_from_path(shortcut_path, name, name_size,
                                           tag, tag_size))
        return false;

    return strcmp(tag, BRIDGE_EMU_TAG) != 0;
}

static bool build_alias_slot_path(const char *shortcut_name, const char *tag,
                                  char *out, size_t out_size)
{
    char shared[SC_MAX_PATH];

    get_shared_userdata_path(shared, sizeof(shared));
    return snprintf(out, out_size, "%s/.minui/%s/%s.m3u.txt",
                    shared, tag, shortcut_name) < (int)out_size;
}

static bool build_real_slot_path(const char *target_path, const char *tag,
                                 char *out, size_t out_size)
{
    char shared[SC_MAX_PATH];
    const char *base = path_basename_local(target_path);

    if (!target_path || base[0] == '\0') return false;

    get_shared_userdata_path(shared, sizeof(shared));
    return snprintf(out, out_size, "%s/.minui/%s/%s.txt",
                    shared, tag, base) < (int)out_size;
}

static bool copy_slot_alias_file(const char *src_path, const char *dst_path)
{
    char *data;
    bool ok;

    data = read_text_file(src_path);
    if (!data) return false;

    ok = ensure_parent_dir_local(dst_path) &&
         write_text_file(dst_path, data) == 0;
    free(data);
    return ok;
}

static int read_manifest_entries(manifest_entry **out_entries, int *out_count)
{
    char manifest_path[SC_MAX_PATH];
    char *data = NULL;
    manifest_entry *entries = NULL;
    int count = 0;
    int cap = 0;
    char *line;
    char *saveptr = NULL;

    *out_entries = NULL;
    *out_count = 0;

    if (!build_manifest_path(manifest_path, sizeof(manifest_path)))
        return -1;

    data = read_text_file(manifest_path);
    if (!data)
        return 0;

    for (line = strtok_r(data, "\n", &saveptr);
         line != NULL;
         line = strtok_r(NULL, "\n", &saveptr)) {
        char *tab;
        manifest_entry *entry;

        if (line[0] == '\0')
            continue;

        tab = strchr(line, '\t');
        if (!tab || tab == line || tab[1] == '\0')
            continue;

        *tab++ = '\0';
        if (count >= cap) {
            int new_cap = cap == 0 ? 8 : cap * 2;
            manifest_entry *new_entries =
                realloc(entries, (size_t)new_cap * sizeof(*entries));
            if (!new_entries) {
                free(entries);
                free(data);
                return -1;
            }
            entries = new_entries;
            cap = new_cap;
        }

        entry = &entries[count++];
        memset(entry, 0, sizeof(*entry));
        copy_cstr_trunc_local(entry->shortcut_path,
                              sizeof(entry->shortcut_path), line);
        copy_cstr_trunc_local(entry->alias_slot_path,
                              sizeof(entry->alias_slot_path), tab);
        trim_trailing_whitespace_local(entry->alias_slot_path);
    }

    free(data);
    *out_entries = entries;
    *out_count = count;
    return 0;
}

static int write_manifest_entries(const manifest_entry *entries, int count)
{
    char manifest_path[SC_MAX_PATH];
    char temp_path[SC_MAX_PATH];
    FILE *f = NULL;

    if (!build_manifest_path(manifest_path, sizeof(manifest_path)))
        return -1;

    if (count <= 0) {
        (void)remove_file_if_exists_local(manifest_path);
        return 0;
    }

    if (!ensure_parent_dir_local(manifest_path))
        return -1;

    if (snprintf(temp_path, sizeof(temp_path), "%s.tmp", manifest_path) >=
        (int)sizeof(temp_path))
        return -1;

    f = fopen(temp_path, "w");
    if (!f) return -1;

    for (int i = 0; i < count; i++) {
        if (entries[i].shortcut_path[0] == '\0' ||
            entries[i].alias_slot_path[0] == '\0')
            continue;
        if (fprintf(f, "%s\t%s\n",
                    entries[i].shortcut_path,
                    entries[i].alias_slot_path) < 0) {
            fclose(f);
            (void)unlink(temp_path);
            return -1;
        }
    }

    if (fclose(f) != 0) {
        (void)unlink(temp_path);
        return -1;
    }

    if (rename(temp_path, manifest_path) != 0) {
        (void)unlink(temp_path);
        return -1;
    }

    return 0;
}

static void remove_manifest_rows_for_shortcut(manifest_entry *entries, int *count,
                                              const char *shortcut_path)
{
    int write_idx = 0;

    for (int i = 0; i < *count; i++) {
        if (strcmp(entries[i].shortcut_path, shortcut_path) == 0) {
            (void)remove_file_if_exists_local(entries[i].alias_slot_path);
            continue;
        }
        if (write_idx != i)
            entries[write_idx] = entries[i];
        write_idx++;
    }

    *count = write_idx;
}

static int upsert_manifest_alias(const char *shortcut_path, const char *alias_slot_path)
{
    manifest_entry *entries = NULL;
    int count = 0;
    int rc;
    bool updated = false;

    rc = read_manifest_entries(&entries, &count);
    if (rc != 0)
        return rc;

    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].shortcut_path, shortcut_path) == 0) {
            copy_cstr_trunc_local(entries[i].alias_slot_path,
                                  sizeof(entries[i].alias_slot_path),
                                  alias_slot_path);
            updated = true;
            break;
        }
    }

    if (!updated) {
        manifest_entry *new_entries =
            realloc(entries, (size_t)(count + 1) * sizeof(*entries));
        if (!new_entries) {
            free(entries);
            return -1;
        }
        entries = new_entries;
        memset(&entries[count], 0, sizeof(entries[count]));
        copy_cstr_trunc_local(entries[count].shortcut_path,
                              sizeof(entries[count].shortcut_path),
                              shortcut_path);
        copy_cstr_trunc_local(entries[count].alias_slot_path,
                              sizeof(entries[count].alias_slot_path),
                              alias_slot_path);
        count++;
    }

    rc = write_manifest_entries(entries, count);
    free(entries);
    return rc;
}

int ensure_resume_hook_installed(void)
{
    char hook_path[SC_MAX_PATH];

    if (!build_resume_hook_path(hook_path, sizeof(hook_path)))
        return -1;

    return write_executable_file_local(hook_path, resume_hook_script) ? 0 : -1;
}

int resume_remove_alias_for_shortcut_path(const char *shortcut_path)
{
    char shortcut_name[SC_MAX_NAME];
    char tag[SC_MAX_TAG];
    char alias_slot_path[SC_MAX_PATH];
    manifest_entry *entries = NULL;
    int count = 0;
    int rc;

    rc = read_manifest_entries(&entries, &count);
    if (rc != 0)
        return rc;

    if (shortcut_path && shortcut_path[0] != '\0')
        remove_manifest_rows_for_shortcut(entries, &count, shortcut_path);

    if (parse_shortcut_name_tag_from_path(shortcut_path,
                                          shortcut_name, sizeof(shortcut_name),
                                          tag, sizeof(tag)) &&
        strcmp(tag, BRIDGE_EMU_TAG) != 0 &&
        build_alias_slot_path(shortcut_name, tag,
                              alias_slot_path, sizeof(alias_slot_path))) {
        (void)remove_file_if_exists_local(alias_slot_path);
    }

    rc = write_manifest_entries(entries, count);
    free(entries);
    return rc;
}

int resume_sync_prune_aliases(void)
{
    manifest_entry *entries = NULL;
    int count = 0;
    int rc;
    int write_idx = 0;

    rc = read_manifest_entries(&entries, &count);
    if (rc != 0)
        return rc;

    for (int i = 0; i < count; i++) {
        char shortcut_name[SC_MAX_NAME];
        char tag[SC_MAX_TAG];

        if (!is_rom_shortcut_path_local(entries[i].shortcut_path,
                                        shortcut_name, sizeof(shortcut_name),
                                        tag, sizeof(tag))) {
            (void)remove_file_if_exists_local(entries[i].alias_slot_path);
            continue;
        }

        if (write_idx != i)
            entries[write_idx] = entries[i];
        write_idx++;
    }

    count = write_idx;
    rc = write_manifest_entries(entries, count);
    free(entries);
    return rc;
}

int resume_sync_for_shortcut_path(const char *shortcut_path)
{
    char shortcut_name[SC_MAX_NAME];
    char tag[SC_MAX_TAG];
    char target_path[SC_MAX_PATH];
    char real_slot_path[SC_MAX_PATH];
    char alias_slot_path[SC_MAX_PATH];

    if (!is_rom_shortcut_path_local(shortcut_path,
                                    shortcut_name, sizeof(shortcut_name),
                                    tag, sizeof(tag)))
        return 0;

    if (!resolve_shortcut_target_path(shortcut_path, shortcut_name,
                                      target_path, sizeof(target_path)))
        return resume_remove_alias_for_shortcut_path(shortcut_path);

    if (!build_real_slot_path(target_path, tag,
                              real_slot_path, sizeof(real_slot_path)) ||
        !build_alias_slot_path(shortcut_name, tag,
                               alias_slot_path, sizeof(alias_slot_path)))
        return -1;

    if (!path_exists_local(real_slot_path))
        return resume_remove_alias_for_shortcut_path(shortcut_path);

    if (!copy_slot_alias_file(real_slot_path, alias_slot_path))
        return -1;

    return upsert_manifest_alias(shortcut_path, alias_slot_path);
}

static int dedup_recent_txt(void)
{
    char shared[SC_MAX_PATH];
    char recent_path[SC_MAX_PATH];
    char *data;
    char **lines = NULL;
    bool *keep = NULL;
    int count = 0;
    int rc = 0;

    get_shared_userdata_path(shared, sizeof(shared));
    if (snprintf(recent_path, sizeof(recent_path), "%s/.minui/recent.txt",
                 shared) >= (int)sizeof(recent_path))
        return -1;

    data = read_text_file(recent_path);
    if (!data)
        return 0;

    /* Count lines. */
    for (const char *p = data; *p; p++)
        if (*p == '\n') count++;
    if (count == 0) { free(data); return 0; }

    lines = calloc((size_t)count, sizeof(*lines));
    keep = calloc((size_t)count, sizeof(*keep));
    if (!lines || !keep) { rc = -1; goto out; }

    /* Parse lines. */
    char *line;
    char *saveptr = NULL;
    int idx = 0;
    for (line = strtok_r(data, "\n", &saveptr);
         line != NULL && idx < count;
         line = strtok_r(NULL, "\n", &saveptr)) {
        if (line[0] == '\0') { keep[idx] = true; idx++; continue; }
        lines[idx] = strdup(line);
        keep[idx] = true;
        idx++;
    }
    count = idx;

    /* Group by base filename and dedup. */
    for (int i = 0; i < count; i++) {
        if (!keep[i] || !lines[i]) continue;

        /* Split line into path and display name. */
        char *tab = strchr(lines[i], '\t');
        if (!tab) continue;
        *tab = '\0';
        const char *path_i = lines[i];
        const char *name_i = tab + 1;

        /* Extract base filename from path (last component). */
        const char *base_i = strrchr(path_i, '/');
        if (!base_i) { *tab = '\t'; continue; }
        base_i++;

        bool i_is_shortcut = strstr(path_i, "/../") != NULL;

        for (int j = i + 1; j < count; j++) {
            if (!keep[j] || !lines[j]) continue;

            char *tab2 = strchr(lines[j], '\t');
            if (!tab2) continue;
            *tab2 = '\0';
            const char *path_j = lines[j];
            const char *name_j = tab2 + 1;

            const char *base_j = strrchr(path_j, '/');
            if (!base_j) { *tab2 = '\t'; continue; }
            base_j++;

            /* Same game if base filename matches. */
            if (strcmp(base_i, base_j) != 0) { *tab2 = '\t'; continue; }
            *tab2 = '\t';

            bool j_is_shortcut = strstr(path_j, "/../") != NULL;

            /* If names differ or one is a shortcut-resolved path, dedup. */
            if (strcmp(name_i, name_j) != 0 || i_is_shortcut != j_is_shortcut) {
                if (i_is_shortcut && !j_is_shortcut)
                    keep[j] = false;
                else if (j_is_shortcut && !i_is_shortcut)
                    keep[i] = false;
                else
                    keep[j] = false;
            }
        }
        *tab = '\t';
    }

    /* Remove non-game entries (tools, bridge shortcuts, etc.). */
    for (int i = 0; i < count; i++) {
        if (!keep[i] || !lines[i]) continue;
        const char *tab = strchr(lines[i], '\t');
        if (!tab) continue;
        size_t path_len = (size_t)(tab - lines[i]);

        const char *tools = strstr(lines[i], "/Tools/");
        if (tools && (size_t)(tools - lines[i]) < path_len) {
            keep[i] = false;
            continue;
        }

        const char *shortcut_tag = strstr(lines[i], "(SHORTCUT)");
        if (shortcut_tag && (size_t)(shortcut_tag - lines[i]) < path_len) {
            keep[i] = false;
            continue;
        }
    }

    /* Count kept entries. */
    int kept = 0;
    for (int i = 0; i < count; i++)
        if (keep[i]) kept++;

    if (kept < count) {
        /* Write back deduplicated recent.txt. */
        char temp_path[SC_MAX_PATH];
        if (snprintf(temp_path, sizeof(temp_path), "%s.tmp", recent_path) >=
            (int)sizeof(temp_path))
            { rc = -1; goto out; }

        FILE *f = fopen(temp_path, "w");
        if (!f) { rc = -1; goto out; }

        for (int i = 0; i < count; i++) {
            if (keep[i] && lines[i]) {
                if (fputs(lines[i], f) == EOF || fputc('\n', f) == EOF) {
                    fclose(f);
                    (void)unlink(temp_path);
                    rc = -1;
                    goto out;
                }
            }
        }

        if (fclose(f) != 0) {
            (void)unlink(temp_path);
            rc = -1;
            goto out;
        }

        if (rename(temp_path, recent_path) != 0) {
            (void)unlink(temp_path);
            rc = -1;
            goto out;
        }
    }

out:
    for (int i = 0; i < count; i++) free(lines[i]);
    free(lines);
    free(keep);
    free(data);
    return rc;
}

int resume_sync_from_hook_env(void)
{
    const char *phase = getenv("HOOK_PHASE");
    const char *type = getenv("HOOK_TYPE");
    const char *last = getenv("HOOK_LAST");
    int rc;

    if (phase && phase[0] && strcmp(phase, "post") != 0)
        return 0;
    if (!type || (strcmp(type, "rom") != 0 && strcmp(type, "tool") != 0))
        return 0;

    rc = resume_sync_prune_aliases();
    if (rc != 0)
        return rc;

    (void)dedup_recent_txt();

    if (strcmp(type, "rom") != 0)
        return 0;

    if (!last || last[0] == '\0')
        return 0;

    return resume_sync_for_shortcut_path(last);
}
