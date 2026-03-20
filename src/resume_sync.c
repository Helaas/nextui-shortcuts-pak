/*
 * resume_sync.c — Mirrors real minarch resume slot files onto ROM shortcut
 *                 names so NextUI can surface "X Resume" for shortcuts.
 */
#include "shortcuts.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(PLATFORM_MAC)
    #define PLATFORM_SUBDIR "tg5040"
#elif defined(PLATFORM_TG5050)
    #define PLATFORM_SUBDIR "tg5050"
#elif defined(PLATFORM_MY355)
    #define PLATFORM_SUBDIR "my355"
#else
    #define PLATFORM_SUBDIR "tg5040"
#endif

#define RESUME_SYNC_AUTO_MARKER_BEGIN \
    "# >>> shortcuts-resume-sync-managed >>>\n"
#define RESUME_SYNC_AUTO_MARKER_END \
    "# <<< shortcuts-resume-sync-managed <<<\n"
#define RESUME_SYNC_MANIFEST_NAME     "resume_aliases.tsv"
#define RESUME_SYNC_PAK_DIRNAME       "Shortcuts.pak"
#define RESUME_SYNC_BINARY_NAME       "shortcuts"
#define RESUME_SYNC_INTERVAL_SECONDS  2
#define RESUME_SYNC_DAEMON_LOCK       "/tmp/shortcuts-resume-sync.daemon.lock"
#define RESUME_SYNC_WORK_LOCK         "/tmp/shortcuts-resume-sync.work.lock"

typedef struct {
    char alias_path[SC_MAX_PATH * 2];
    char real_path[SC_MAX_PATH * 2];
} resume_alias_mapping;

static volatile sig_atomic_t g_resume_sync_stop = 0;

static bool copy_cstr_exact_local(char *out, size_t out_size, const char *src)
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

static bool join_path_local(char *out, size_t out_size,
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

static void copy_with_suffix_trunc_local(char *out, size_t out_size,
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

static const char *basename_ptr(const char *path)
{
    const char *slash;

    if (!path || path[0] == '\0')
        return path;

    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool build_sdcard_root(char *out, size_t out_size)
{
#if defined(PLATFORM_MAC)
    char cwd[SC_MAX_PATH];

    if (getcwd(cwd, sizeof(cwd)))
        return snprintf(out, out_size, "%s/mock_sdcard", cwd) < (int)out_size;
    return snprintf(out, out_size, "./mock_sdcard") < (int)out_size;
#else
    const char *sd = getenv("SDCARD_PATH");
    return snprintf(out, out_size, "%s", sd && sd[0] ? sd : "/mnt/SDCARD") <
           (int)out_size;
#endif
}

static bool build_shared_userdata_root(char *out, size_t out_size)
{
#if defined(PLATFORM_MAC)
    char sd_root[SC_MAX_PATH];

    if (!build_sdcard_root(sd_root, sizeof(sd_root)))
        return false;
    return snprintf(out, out_size, "%s/.userdata/shared", sd_root) <
           (int)out_size;
#else
    const char *shared = getenv("SHARED_USERDATA_PATH");
    char sd_root[SC_MAX_PATH];

    if (shared && shared[0])
        return snprintf(out, out_size, "%s", shared) < (int)out_size;
    if (!build_sdcard_root(sd_root, sizeof(sd_root)))
        return false;
    return snprintf(out, out_size, "%s/.userdata/shared", sd_root) <
           (int)out_size;
#endif
}

static bool build_device_userdata_root(char *out, size_t out_size)
{
#if defined(PLATFORM_MAC)
    char sd_root[SC_MAX_PATH];

    if (!build_sdcard_root(sd_root, sizeof(sd_root)))
        return false;
    return snprintf(out, out_size, "%s/.userdata/%s", sd_root,
                    PLATFORM_SUBDIR) < (int)out_size;
#else
    const char *userdata = getenv("USERDATA_PATH");
    char sd_root[SC_MAX_PATH];

    if (userdata && userdata[0])
        return snprintf(out, out_size, "%s", userdata) < (int)out_size;
    if (!build_sdcard_root(sd_root, sizeof(sd_root)))
        return false;
    return snprintf(out, out_size, "%s/.userdata/%s", sd_root,
                    PLATFORM_SUBDIR) < (int)out_size;
#endif
}

static bool build_manifest_path(char *out, size_t out_size)
{
    char shared_root[SC_MAX_PATH];
    char shortcuts_dir[SC_MAX_PATH * 2];

    if (!build_shared_userdata_root(shared_root, sizeof(shared_root)) ||
        !join_path_local(shortcuts_dir, sizeof(shortcuts_dir),
                         shared_root, "Shortcuts"))
        return false;

    return join_path_local(out, out_size, shortcuts_dir,
                           RESUME_SYNC_MANIFEST_NAME);
}

static bool build_auto_path(char *out, size_t out_size)
{
    char userdata_root[SC_MAX_PATH];

    if (!build_device_userdata_root(userdata_root, sizeof(userdata_root)))
        return false;
    return join_path_local(out, out_size, userdata_root, "auto.sh");
}

static bool build_helper_path(char *out, size_t out_size)
{
    char sd_root[SC_MAX_PATH];
    char tools_root[SC_MAX_PATH * 2];
    char pak_dir[SC_MAX_PATH * 2];

    if (!build_sdcard_root(sd_root, sizeof(sd_root)) ||
        !join_path_local(tools_root, sizeof(tools_root), sd_root, "Tools") ||
        !join_path_local(tools_root, sizeof(tools_root), tools_root,
                         PLATFORM_SUBDIR) ||
        !join_path_local(pak_dir, sizeof(pak_dir), tools_root,
                         RESUME_SYNC_PAK_DIRNAME))
        return false;

    return join_path_local(out, out_size, pak_dir, RESUME_SYNC_BINARY_NAME);
}

static bool build_log_dir_path(char *out, size_t out_size)
{
    char shared_root[SC_MAX_PATH];

    if (!build_shared_userdata_root(shared_root, sizeof(shared_root)))
        return false;
    return join_path_local(out, out_size, shared_root, "logs");
}

static bool build_log_file_path(char *out, size_t out_size)
{
    char log_dir[SC_MAX_PATH * 2];

    if (!build_log_dir_path(log_dir, sizeof(log_dir)))
        return false;
    return join_path_local(out, out_size, log_dir,
                           "shortcuts-resume-sync.txt");
}

static bool build_minui_dir_path(const char *tag, char *out, size_t out_size)
{
    char shared_root[SC_MAX_PATH];
    char minui_root[SC_MAX_PATH * 2];

    if (!tag || tag[0] == '\0')
        return false;
    if (!build_shared_userdata_root(shared_root, sizeof(shared_root)) ||
        !join_path_local(minui_root, sizeof(minui_root), shared_root, ".minui"))
        return false;

    return join_path_local(out, out_size, minui_root, tag);
}

static int ensure_parent_dir_exists_local(const char *path)
{
    char parent[SC_MAX_PATH * 2];
    char *slash;

    if (!copy_cstr_exact_local(parent, sizeof(parent), path))
        return -1;

    slash = strrchr(parent, '/');
    if (!slash)
        return 0;
    if (slash == parent) {
        slash[1] = '\0';
        return 0;
    }

    *slash = '\0';
    return ensure_dir_exists(parent);
}

static int write_text_file_atomic_local(const char *path,
                                        const char *content,
                                        size_t len)
{
    char temp_path[SC_MAX_PATH * 2];
    FILE *file;

    if (!path || !content)
        return -1;
    if (snprintf(temp_path, sizeof(temp_path), "%s.tmp", path) >=
        (int)sizeof(temp_path))
        return -1;
    if (ensure_parent_dir_exists_local(path) != 0)
        return -1;

    file = fopen(temp_path, "wb");
    if (!file)
        return -1;

    if (len > 0 && fwrite(content, 1, len, file) != len) {
        fclose(file);
        unlink(temp_path);
        return -1;
    }
    if (fclose(file) != 0) {
        unlink(temp_path);
        return -1;
    }
    if (rename(temp_path, path) != 0) {
        unlink(temp_path);
        return -1;
    }
    return 0;
}

static char *read_text_file_or_empty_local(const char *path, size_t *out_len)
{
    char *data;

    if (out_len)
        *out_len = 0;

    data = read_text_file(path);
    if (data) {
        if (out_len)
            *out_len = strlen(data);
        return data;
    }

    data = malloc(1);
    if (!data)
        return NULL;
    data[0] = '\0';
    return data;
}

static int append_mapping(resume_alias_mapping **arr, int *count, int *cap,
                          const char *alias_path, const char *real_path)
{
    resume_alias_mapping *grown;

    if (!arr || !count || !cap || !alias_path || !real_path)
        return -1;

    if (*count >= *cap) {
        int new_cap = (*cap == 0) ? 8 : (*cap * 2);
        grown = realloc(*arr, (size_t)new_cap * sizeof(**arr));
        if (!grown)
            return -1;
        *arr = grown;
        *cap = new_cap;
    }

    memset(&(*arr)[*count], 0, sizeof(**arr));
    if (!copy_cstr_exact_local((*arr)[*count].alias_path,
                               sizeof((*arr)[*count].alias_path),
                               alias_path) ||
        !copy_cstr_exact_local((*arr)[*count].real_path,
                               sizeof((*arr)[*count].real_path),
                               real_path))
        return -1;

    (*count)++;
    return 0;
}

static int collect_current_alias_mappings(resume_alias_mapping **out,
                                          int *out_count)
{
    shortcut_entry *shortcuts = NULL;
    resume_alias_mapping *mappings = NULL;
    int shortcut_count = 0;
    int count = 0;
    int cap = 0;
    int rc = -1;

    if (!out || !out_count)
        return -1;

    *out = NULL;
    *out_count = 0;

    if (scan_shortcuts(&shortcuts, &shortcut_count) != 0)
        goto cleanup;

    for (int i = 0; i < shortcut_count; i++) {
        char minui_dir[SC_MAX_PATH * 2];
        char alias_leaf[SC_MAX_PATH];
        char real_leaf[SC_MAX_PATH];
        char alias_path[SC_MAX_PATH * 2];
        char real_path[SC_MAX_PATH * 2];
        const char *real_base;

        if (shortcuts[i].is_tool || shortcuts[i].target_path[0] == '\0' ||
            shortcuts[i].tag[0] == '\0')
            continue;

        real_base = basename_ptr(shortcuts[i].target_path);
        if (!real_base || real_base[0] == '\0')
            continue;
        if (!build_minui_dir_path(shortcuts[i].tag, minui_dir,
                                  sizeof(minui_dir)))
            continue;

        copy_with_suffix_trunc_local(alias_leaf, sizeof(alias_leaf),
                                     shortcuts[i].name, ".m3u.txt");
        copy_with_suffix_trunc_local(real_leaf, sizeof(real_leaf),
                                     real_base, ".txt");

        if (!join_path_local(alias_path, sizeof(alias_path),
                             minui_dir, alias_leaf) ||
            !join_path_local(real_path, sizeof(real_path),
                             minui_dir, real_leaf))
            continue;
        if (strcmp(alias_path, real_path) == 0)
            continue;

        if (append_mapping(&mappings, &count, &cap, alias_path, real_path) !=
            0)
            goto cleanup;
    }

    *out = mappings;
    *out_count = count;
    mappings = NULL;
    rc = 0;

cleanup:
    free(mappings);
    free(shortcuts);
    return rc;
}

static int load_manifest(resume_alias_mapping **out, int *out_count)
{
    char manifest_path[SC_MAX_PATH * 2];
    char *data = NULL;
    resume_alias_mapping *mappings = NULL;
    int count = 0;
    int cap = 0;
    int rc = -1;

    if (!out || !out_count)
        return -1;

    *out = NULL;
    *out_count = 0;

    if (!build_manifest_path(manifest_path, sizeof(manifest_path)))
        return -1;

    data = read_text_file(manifest_path);
    if (!data) {
        rc = 0;
        goto cleanup;
    }

    char *cursor = data;
    while (cursor && *cursor) {
        char *line = cursor;
        char *next = strchr(cursor, '\n');
        char *tab;

        if (next) {
            *next = '\0';
            cursor = next + 1;
        } else {
            cursor = NULL;
        }

        while (*line == '\r' || *line == '\n')
            line++;
        if (*line == '\0')
            continue;

        tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab++ = '\0';
        if (*line == '\0' || *tab == '\0')
            continue;

        if (append_mapping(&mappings, &count, &cap, line, tab) != 0)
            goto cleanup;
    }

    *out = mappings;
    *out_count = count;
    mappings = NULL;
    rc = 0;

cleanup:
    free(mappings);
    free(data);
    return rc;
}

static bool mapping_matches(const resume_alias_mapping *mappings, int count,
                            const char *alias_path, const char *real_path)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(mappings[i].alias_path, alias_path) == 0 &&
            strcmp(mappings[i].real_path, real_path) == 0)
            return true;
    }
    return false;
}

static int write_manifest(const resume_alias_mapping *mappings, int count)
{
    char manifest_path[SC_MAX_PATH * 2];
    char *buffer;
    size_t total_len = 0;
    size_t pos = 0;
    int rc;

    if (!build_manifest_path(manifest_path, sizeof(manifest_path)))
        return -1;

    for (int i = 0; i < count; i++) {
        total_len += strlen(mappings[i].alias_path) + 1 +
                     strlen(mappings[i].real_path) + 1;
    }

    buffer = malloc(total_len + 1);
    if (!buffer)
        return -1;

    for (int i = 0; i < count; i++) {
        int written = snprintf(buffer + pos, total_len + 1 - pos, "%s\t%s\n",
                               mappings[i].alias_path, mappings[i].real_path);
        if (written < 0 || (size_t)written >= total_len + 1 - pos) {
            free(buffer);
            return -1;
        }
        pos += (size_t)written;
    }
    buffer[pos] = '\0';

    rc = write_text_file_atomic_local(manifest_path, buffer, pos);
    free(buffer);
    return rc;
}

static int sync_alias_slot_file(const resume_alias_mapping *mapping)
{
    char *real_data = NULL;
    char *alias_data = NULL;
    int rc = 0;

    if (!mapping)
        return -1;

    if (access(mapping->real_path, F_OK) != 0) {
        if (strcmp(mapping->alias_path, mapping->real_path) != 0)
            (void)unlink(mapping->alias_path);
        return 0;
    }

    real_data = read_text_file(mapping->real_path);
    if (!real_data)
        return -1;

    alias_data = read_text_file(mapping->alias_path);
    if (alias_data && strcmp(alias_data, real_data) == 0)
        goto cleanup;

    rc = write_text_file_atomic_local(mapping->alias_path, real_data,
                                      strlen(real_data));

cleanup:
    free(alias_data);
    free(real_data);
    return rc;
}

static int perform_resume_sync(void)
{
    resume_alias_mapping *current = NULL;
    resume_alias_mapping *previous = NULL;
    int current_count = 0;
    int previous_count = 0;
    int had_error = 0;

    if (collect_current_alias_mappings(&current, &current_count) != 0)
        had_error = 1;
    if (had_error)
        goto cleanup;
    if (load_manifest(&previous, &previous_count) != 0)
        had_error = 1;
    if (had_error)
        goto cleanup;

    for (int i = 0; i < previous_count; i++) {
        if (!mapping_matches(current, current_count,
                             previous[i].alias_path,
                             previous[i].real_path) &&
            strcmp(previous[i].alias_path, previous[i].real_path) != 0) {
            if (unlink(previous[i].alias_path) != 0 && errno != ENOENT)
                had_error = 1;
        }
    }

    for (int i = 0; i < current_count; i++) {
        if (sync_alias_slot_file(&current[i]) != 0)
            had_error = 1;
    }

    if (write_manifest(current, current_count) != 0)
        had_error = 1;

cleanup:
    free(current);
    free(previous);
    return had_error ? -1 : 0;
}

static int open_lock_fd(const char *path)
{
    int fd;

    fd = open(path, O_CREAT | O_RDWR, 0644);
    return fd;
}

static int lock_fd(int fd, bool block)
{
    struct flock fl = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0,
    };
    int cmd = block ? F_SETLKW : F_SETLK;

    if (fd < 0)
        return -1;

    for (;;) {
        if (fcntl(fd, cmd, &fl) == 0)
            return 0;
        if (errno == EINTR)
            continue;
        return -1;
    }
}

static void unlock_and_close_fd(int fd)
{
    struct flock fl = {
        .l_type = F_UNLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0,
    };

    if (fd < 0)
        return;

    (void)fcntl(fd, F_SETLK, &fl);
    close(fd);
}

static void handle_resume_sync_signal(int sig)
{
    (void)sig;
    g_resume_sync_stop = 1;
}

static int upsert_auto_block(void)
{
    char auto_path[SC_MAX_PATH * 2];
    char helper_path[SC_MAX_PATH * 2];
    char log_dir[SC_MAX_PATH * 2];
    char log_file[SC_MAX_PATH * 2];
    char block[(SC_MAX_PATH * 8) + 1024];
    char *existing = NULL;
    char *begin;
    char *end;
    char *new_content = NULL;
    size_t existing_len;
    size_t block_len;
    size_t prefix_len;
    size_t suffix_len;
    size_t extra_newline = 0;
    int rc = -1;

    if (!build_auto_path(auto_path, sizeof(auto_path)) ||
        !build_helper_path(helper_path, sizeof(helper_path)) ||
        !build_log_dir_path(log_dir, sizeof(log_dir)) ||
        !build_log_file_path(log_file, sizeof(log_file)))
        return -1;

    block_len = (size_t)snprintf(block, sizeof(block),
        "%s"
        "SHORTCUTS_RESUME_HELPER=\"%s\"\n"
        "SHORTCUTS_RESUME_LOG_DIR=\"%s\"\n"
        "SHORTCUTS_RESUME_LOG_FILE=\"%s\"\n"
        "if [ -x \"$SHORTCUTS_RESUME_HELPER\" ]; then\n"
        "    mkdir -p \"$SHORTCUTS_RESUME_LOG_DIR\" > /dev/null 2>&1 || true\n"
        "    \"$SHORTCUTS_RESUME_HELPER\" --resume-sync-daemon >>"
        "\"$SHORTCUTS_RESUME_LOG_FILE\" 2>&1 &\n"
        "fi\n"
        "%s",
        RESUME_SYNC_AUTO_MARKER_BEGIN,
        helper_path,
        log_dir,
        log_file,
        RESUME_SYNC_AUTO_MARKER_END);
    if (block_len >= sizeof(block))
        return -1;

    existing = read_text_file_or_empty_local(auto_path, &existing_len);
    if (!existing)
        return -1;

    begin = strstr(existing, RESUME_SYNC_AUTO_MARKER_BEGIN);
    end = begin ? strstr(begin, RESUME_SYNC_AUTO_MARKER_END) : NULL;

    if (begin && end) {
        prefix_len = (size_t)(begin - existing);
        end += strlen(RESUME_SYNC_AUTO_MARKER_END);
        if (*end == '\r')
            ++end;
        if (*end == '\n')
            ++end;
        suffix_len = existing_len - (size_t)(end - existing);
    } else if (begin) {
        prefix_len = (size_t)(begin - existing);
        suffix_len = 0;
    } else {
        prefix_len = existing_len;
        suffix_len = 0;
        if (existing_len > 0 && existing[existing_len - 1] != '\n')
            extra_newline = 1;
    }

    new_content = malloc(prefix_len + extra_newline + block_len + suffix_len + 1);
    if (!new_content)
        goto cleanup;

    memcpy(new_content, existing, prefix_len);
    if (extra_newline)
        new_content[prefix_len] = '\n';
    memcpy(new_content + prefix_len + extra_newline, block, block_len);
    if (suffix_len > 0)
        memcpy(new_content + prefix_len + extra_newline + block_len,
               existing + existing_len - suffix_len, suffix_len);
    new_content[prefix_len + extra_newline + block_len + suffix_len] = '\0';

    rc = write_text_file_atomic_local(auto_path, new_content,
                                      prefix_len + extra_newline + block_len +
                                      suffix_len);
    if (rc == 0)
        (void)chmod(auto_path, 0755);

cleanup:
    free(new_content);
    free(existing);
    return rc;
}

void ensure_resume_sync_autostart(void)
{
    if (upsert_auto_block() != 0)
        ap_log("resume_sync: failed to update auto.sh");
}

void start_resume_sync_helper(const char *argv0)
{
#if defined(PLATFORM_MAC)
    (void)argv0;
    return;
#else
    pid_t pid;
    int devnull;

    if (!argv0 || argv0[0] == '\0')
        return;

    pid = fork();
    if (pid != 0)
        return;

    (void)setsid();

    devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        (void)dup2(devnull, STDIN_FILENO);
        (void)dup2(devnull, STDOUT_FILENO);
        (void)dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO)
            close(devnull);
    }

    execl(argv0, argv0, "--resume-sync-daemon", (char *)NULL);
    _exit(127);
#endif
}

int resume_sync_once(void)
{
    int lock_fd_local;
    int rc;

    lock_fd_local = open_lock_fd(RESUME_SYNC_WORK_LOCK);
    if (lock_fd_local < 0)
        return -1;
    if (lock_fd(lock_fd_local, true) != 0) {
        unlock_and_close_fd(lock_fd_local);
        return -1;
    }

    rc = perform_resume_sync();
    unlock_and_close_fd(lock_fd_local);
    return rc;
}

int resume_sync_daemon(void)
{
    int daemon_lock_fd;

    daemon_lock_fd = open_lock_fd(RESUME_SYNC_DAEMON_LOCK);
    if (daemon_lock_fd < 0)
        return 1;
    if (lock_fd(daemon_lock_fd, false) != 0) {
        unlock_and_close_fd(daemon_lock_fd);
        return 0;
    }

    signal(SIGINT, handle_resume_sync_signal);
    signal(SIGTERM, handle_resume_sync_signal);

    while (!g_resume_sync_stop) {
        if (resume_sync_once() != 0)
            ap_log("resume_sync: sync iteration failed");

        for (int i = 0; i < RESUME_SYNC_INTERVAL_SECONDS &&
                        !g_resume_sync_stop; i++) {
            sleep(1);
        }
    }

    unlock_and_close_fd(daemon_lock_fd);
    return 0;
}
