#include "shortcuts.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <sqlite3.h>

static bool build_folder_name(sc_position pos, const char *display,
                              const char *tag, char *out, int out_size)
{
    int n;
    switch (pos) {
    case SC_POS_TOP:
        n = snprintf(out, out_size, TOP_PREFIX "%s (%s)", display, tag);
        break;
    case SC_POS_ALPHA:
        n = snprintf(out, out_size, "%s (%s)", display, tag);
        break;
    default:
        n = snprintf(out, out_size, SHORTCUT_PREFIX "%s (%s)", display, tag);
        break;
    }
    return n >= 0 && n < out_size;
}

extern char g_last_generated_art_src_path[];
extern char g_last_generated_art_dest_folder[];
extern int g_generate_artwork_bg_call_count;
void reset_generate_artwork_bg_stub(void);

typedef struct {
    const char *name;
    bool (*fn)(void);
} test_case;

typedef struct {
    char original_cwd[SC_MAX_PATH];
    char temp_root[SC_MAX_PATH];
} test_env;

#define CHECK(cond, fmt, ...)                                                  \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL: " fmt "\n", ##__VA_ARGS__);                 \
            goto cleanup;                                                      \
        }                                                                      \
    } while (0)

static bool make_dir_recursive(const char *path)
{
    char tmp[SC_MAX_PATH];
    size_t len;

    if (!path || path[0] == '\0') return false;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (len == 0 || len >= sizeof(tmp)) return false;

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
            return false;
        *p = '/';
    }

    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

static bool write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (!f) return false;
    if (content && fputs(content, f) == EOF) {
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

static bool touch_file(const char *path)
{
    return write_file(path, "");
}

static bool path_exists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

static bool path_is_dir(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool file_contents_equal(const char *path, const char *expected)
{
    bool ok = false;
    char *data = read_text_file(path);

    if (!data) return false;
    ok = strcmp(data, expected) == 0;
    free(data);
    return ok;
}

static bool sqlite_exec_sql(const char *db_path, const char *sql)
{
    sqlite3 *db = NULL;
    char *err = NULL;
    bool ok = false;

    if (sqlite3_open(db_path, &db) != SQLITE_OK)
        goto out;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK)
        goto out;

    ok = true;

out:
    sqlite3_free(err);
    if (db) sqlite3_close(db);
    return ok;
}

static bool sqlite_query_int(const char *db_path, const char *sql, int *out)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    bool ok = false;

    if (!out) return false;
    *out = 0;

    if (sqlite3_open(db_path, &db) != SQLITE_OK)
        goto out;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        goto out;
    if (sqlite3_step(stmt) != SQLITE_ROW)
        goto out;

    *out = sqlite3_column_int(stmt, 0);
    ok = true;

out:
    if (stmt) sqlite3_finalize(stmt);
    if (db) sqlite3_close(db);
    return ok;
}

static bool write_executable_file(const char *path, const char *content)
{
    return write_file(path, content) && chmod(path, 0755) == 0;
}

static bool run_command_success(const char *cmd)
{
    return system(cmd) == 0;
}

static bool remove_tree(const char *path)
{
    struct stat st;

    if (lstat(path, &st) != 0)
        return errno == ENOENT;

    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
        return unlink(path) == 0;

    DIR *d = opendir(path);
    if (!d) return false;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        char child[SC_MAX_PATH];

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        if (snprintf(child, sizeof(child), "%s/%s", path, ent->d_name) >=
            (int)sizeof(child)) {
            closedir(d);
            return false;
        }

        if (!remove_tree(child)) {
            closedir(d);
            return false;
        }
    }

    closedir(d);
    return rmdir(path) == 0;
}

static bool setup_test_env(test_env *env)
{
    char template[] = "/tmp/shortcuts-scan-tests-XXXXXX";
    char *temp_root;

    memset(env, 0, sizeof(*env));

    if (!getcwd(env->original_cwd, sizeof(env->original_cwd)))
        return false;

    temp_root = mkdtemp(template);
    if (!temp_root) return false;

    snprintf(env->temp_root, sizeof(env->temp_root), "%s", temp_root);
    if (chdir(env->temp_root) != 0)
        return false;

    return make_dir_recursive("mock_sdcard/Roms") &&
           make_dir_recursive("mock_sdcard/.userdata/shared") &&
           make_dir_recursive("mock_sdcard/.userdata/tg5040/logs");
}

static void teardown_test_env(test_env *env)
{
    if (env->original_cwd[0] != '\0')
        (void)chdir(env->original_cwd);
    if (env->temp_root[0] != '\0')
        (void)remove_tree(env->temp_root);
}

static const console_dir *find_console_by_name(const console_dir *consoles,
                                               int count,
                                               const char *name)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(consoles[i].name, name) == 0)
            return &consoles[i];
    }
    return NULL;
}

static const rom_file *find_rom_by_display(const rom_file *roms,
                                           int count,
                                           const char *display)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(roms[i].display, display) == 0)
            return &roms[i];
    }
    return NULL;
}

static const rom_file *find_rom_by_name(const rom_file *roms,
                                        int count,
                                        const char *name)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(roms[i].name, name) == 0)
            return &roms[i];
    }
    return NULL;
}

static const shortcut_entry *find_shortcut_by_display(
    const shortcut_entry *shortcuts, int count, const char *display)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(shortcuts[i].display, display) == 0)
            return &shortcuts[i];
    }
    return NULL;
}

static const tool_pak *find_tool_by_name(const tool_pak *tools,
                                         int count,
                                         const char *name)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(tools[i].name, name) == 0)
            return &tools[i];
    }
    return NULL;
}

static bool build_absolute_path(const test_env *env, const char *relative_path,
                                char *out, size_t out_size)
{
    char cwd[SC_MAX_PATH];

    (void)env;
    if (!getcwd(cwd, sizeof(cwd)))
        return false;
    return snprintf(out, out_size, "%s/%s", cwd, relative_path) <
           (int)out_size;
}

static bool build_resume_manifest_path(char *out, size_t out_size)
{
    return snprintf(out, out_size,
                    "mock_sdcard/.userdata/shared/Shortcuts/resume_aliases.tsv") <
           (int)out_size;
}

static bool build_resume_hook_path(char *out, size_t out_size)
{
    return snprintf(out, out_size,
                    "mock_sdcard/.userdata/tg5040/.hooks/post-launch.d/shortcuts-resume.sh") <
           (int)out_size;
}

static bool build_alias_slot_path(const char *shortcut_name, const char *tag,
                                  char *out, size_t out_size)
{
    return snprintf(out, out_size,
                    "mock_sdcard/.userdata/shared/.minui/%s/%s.m3u.txt",
                    tag, shortcut_name) < (int)out_size;
}

static bool build_real_slot_path(const char *tag, const char *slot_name,
                                 char *out, size_t out_size)
{
    return snprintf(out, out_size,
                    "mock_sdcard/.userdata/shared/.minui/%s/%s.txt",
                    tag, slot_name) < (int)out_size;
}

static bool manifest_contains_entry(const char *shortcut_path,
                                    const char *alias_path)
{
    char manifest_path[SC_MAX_PATH];
    char rel_rel_line[SC_MAX_PATH * 2];
    char rel_abs_line[SC_MAX_PATH * 2];
    char abs_rel_line[SC_MAX_PATH * 2];
    char abs_abs_line[SC_MAX_PATH * 2];
    char abs_shortcut_path[SC_MAX_PATH];
    char abs_alias_path[SC_MAX_PATH];
    char *data;
    bool ok = false;
    bool have_abs_shortcut = false;
    bool have_abs_alias = false;

    if (!build_resume_manifest_path(manifest_path, sizeof(manifest_path)))
        return false;
    if (!path_exists(manifest_path))
        return false;
    if (snprintf(rel_rel_line, sizeof(rel_rel_line), "%s\t%s\n",
                 shortcut_path, alias_path) >= (int)sizeof(rel_rel_line))
        return false;

    if (shortcut_path[0] == '/') {
        snprintf(abs_shortcut_path, sizeof(abs_shortcut_path), "%s",
                 shortcut_path);
        have_abs_shortcut = true;
    } else if (getcwd(abs_shortcut_path, sizeof(abs_shortcut_path)) != NULL) {
        size_t len = strlen(abs_shortcut_path);
        if (snprintf(abs_shortcut_path + len,
                     sizeof(abs_shortcut_path) - len,
                     "/%s", shortcut_path) <
            (int)(sizeof(abs_shortcut_path) - len))
            have_abs_shortcut = true;
    }

    if (alias_path[0] == '/') {
        snprintf(abs_alias_path, sizeof(abs_alias_path), "%s", alias_path);
        have_abs_alias = true;
    } else if (getcwd(abs_alias_path, sizeof(abs_alias_path)) != NULL) {
        size_t len = strlen(abs_alias_path);
        if (snprintf(abs_alias_path + len,
                     sizeof(abs_alias_path) - len,
                     "/%s", alias_path) <
            (int)(sizeof(abs_alias_path) - len))
            have_abs_alias = true;
    }

    rel_abs_line[0] = '\0';
    abs_rel_line[0] = '\0';
    abs_abs_line[0] = '\0';
    if (have_abs_alias &&
        snprintf(rel_abs_line, sizeof(rel_abs_line), "%s\t%s\n",
                 shortcut_path, abs_alias_path) >= (int)sizeof(rel_abs_line))
        return false;
    if (have_abs_shortcut &&
        snprintf(abs_rel_line, sizeof(abs_rel_line), "%s\t%s\n",
                 abs_shortcut_path, alias_path) >= (int)sizeof(abs_rel_line))
        return false;
    if (have_abs_shortcut && have_abs_alias &&
        snprintf(abs_abs_line, sizeof(abs_abs_line), "%s\t%s\n",
                 abs_shortcut_path, abs_alias_path) >= (int)sizeof(abs_abs_line))
        return false;

    data = read_text_file(manifest_path);
    if (!data) return false;
    ok = strstr(data, rel_rel_line) != NULL ||
         (rel_abs_line[0] != '\0' && strstr(data, rel_abs_line) != NULL) ||
         (abs_rel_line[0] != '\0' && strstr(data, abs_rel_line) != NULL) ||
         (abs_abs_line[0] != '\0' && strstr(data, abs_abs_line) != NULL);
    free(data);
    return ok;
}

static void clear_hook_env(void)
{
    unsetenv("HOOK_PHASE");
    unsetenv("HOOK_TYPE");
    unsetenv("HOOK_LAST");
}

static bool set_hook_env(const char *type, const char *last_path)
{
    if (setenv("HOOK_PHASE", "post", 1) != 0)
        return false;
    if (setenv("HOOK_TYPE", type ? type : "", 1) != 0)
        return false;
    if (setenv("HOOK_LAST", last_path ? last_path : "", 1) != 0)
        return false;
    return true;
}

static bool create_rom_shortcut_fixture(const char *display_name,
                                        const char *tag,
                                        sc_position pos,
                                        const char *target_path,
                                        char *out_folder_name,
                                        size_t out_folder_name_size,
                                        char *out_folder_path,
                                        size_t out_folder_path_size)
{
    char folder_name[SC_MAX_NAME];
    char folder_path[SC_MAX_PATH];
    char m3u_path[SC_MAX_PATH * 2];
    char marker_path[SC_MAX_PATH * 2];

    if (!build_folder_name(pos, display_name, tag,
                           folder_name, sizeof(folder_name)))
        return false;
    if (snprintf(folder_path, sizeof(folder_path),
                 "mock_sdcard/Roms/%s", folder_name) >=
        (int)sizeof(folder_path))
        return false;
    if (!make_dir_recursive(folder_path))
        return false;
    if (snprintf(m3u_path, sizeof(m3u_path), "%s/%s.m3u",
                 folder_path, folder_name) >= (int)sizeof(m3u_path))
        return false;
    if (snprintf(marker_path, sizeof(marker_path), "%s/.shortcut",
                 folder_path) >= (int)sizeof(marker_path))
        return false;
    if (!write_file(m3u_path, target_path))
        return false;
    if (!write_file(marker_path, display_name))
        return false;

    if (out_folder_name && out_folder_name_size > 0)
        snprintf(out_folder_name, out_folder_name_size, "%s", folder_name);
    if (out_folder_path && out_folder_path_size > 0)
        snprintf(out_folder_path, out_folder_path_size, "%s", folder_path);
    return true;
}

static bool create_tool_shortcut_fixture(const char *display_name,
                                         sc_position pos,
                                         char *out_folder_path,
                                         size_t folder_path_size)
{
    char folder_name[SC_MAX_NAME];
    char folder_path[SC_MAX_PATH];
    char m3u_path[SC_MAX_PATH];
    char marker_path[SC_MAX_PATH];
    char target_path[SC_MAX_PATH];

    if (!build_folder_name(pos, display_name, BRIDGE_EMU_TAG,
                           folder_name, sizeof(folder_name)))
        return false;
    if (snprintf(folder_path, sizeof(folder_path), "mock_sdcard/Roms/%s",
                 folder_name) >= (int)sizeof(folder_path))
        return false;
    if (!make_dir_recursive(folder_path))
        return false;
    if (snprintf(m3u_path, sizeof(m3u_path), "%s/%s.m3u",
                 folder_path, folder_name) >= (int)sizeof(m3u_path))
        return false;
    if (snprintf(marker_path, sizeof(marker_path), "%s/.shortcut",
                 folder_path) >= (int)sizeof(marker_path))
        return false;
    if (snprintf(target_path, sizeof(target_path), "%s/target",
                 folder_path) >= (int)sizeof(target_path))
        return false;
    if (!write_file(m3u_path, "../../Tools/tg5040/Foo.pak"))
        return false;
    if (!write_file(marker_path, display_name))
        return false;
    if (!write_file(target_path, "/mnt/SDCARD/Tools/tg5040/Foo.pak"))
        return false;

    if (out_folder_path && folder_path_size > 0)
        snprintf(out_folder_path, folder_path_size, "%s", folder_path);
    return true;
}

static bool test_sidecar_only_console_is_hidden(void)
{
    test_env env = {0};
    console_dir *consoles = NULL;
    int count = 0;
    bool ok = false;

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(make_dir_recursive("mock_sdcard/Roms/Sidecars Only (SC)/.media"),
          "mkdir sidecar fixture failed");
    CHECK(touch_file("mock_sdcard/Roms/Sidecars Only (SC)/manual.pdf"),
          "create pdf failed");
    CHECK(touch_file("mock_sdcard/Roms/Sidecars Only (SC)/cover.png"),
          "create image failed");
    CHECK(touch_file("mock_sdcard/Roms/Sidecars Only (SC)/gamelist.xml"),
          "create metadata failed");
    CHECK(touch_file("mock_sdcard/Roms/Sidecars Only (SC)/readme.txt"),
          "create text failed");
    CHECK(touch_file("mock_sdcard/Roms/Sidecars Only (SC)/save.sav"),
          "create save failed");
    CHECK(touch_file("mock_sdcard/Roms/Sidecars Only (SC)/preview.mp4"),
          "create preview failed");
    CHECK(touch_file("mock_sdcard/Roms/Sidecars Only (SC)/.DS_Store"),
          "create ds_store failed");
    CHECK(make_dir_recursive("mock_sdcard/Roms/Sidecars Only (SC)/.Spotlight-V100"),
          "mkdir spotlight failed");

    CHECK(scan_console_dirs(false, &consoles, &count) == 0,
          "scan_console_dirs(false) failed");
    CHECK(count == 0, "expected no visible consoles, got %d", count);
    free(consoles);
    consoles = NULL;

    CHECK(scan_console_dirs(true, &consoles, &count) == 0,
          "scan_console_dirs(true) failed");
    CHECK(count == 0, "expected no hidden consoles with sidecars only, got %d",
          count);

    ok = true;

cleanup:
    free(consoles);
    teardown_test_env(&env);
    return ok;
}

static bool test_real_rom_ignores_sidecars(void)
{
    test_env env = {0};
    console_dir *consoles = NULL;
    rom_file *roms = NULL;
    int count = 0;
    bool ok = false;
    const console_dir *console = NULL;
    const rom_file *rom = NULL;

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(make_dir_recursive("mock_sdcard/Roms/Has Games (ROM)"),
          "mkdir rom fixture failed");
    CHECK(touch_file("mock_sdcard/Roms/Has Games (ROM)/Actual Game.chd"),
          "create rom failed");
    CHECK(touch_file("mock_sdcard/Roms/Has Games (ROM)/Actual Game-manual.pdf"),
          "create manual failed");
    CHECK(touch_file("mock_sdcard/Roms/Has Games (ROM)/Actual Game-image.png"),
          "create image failed");
    CHECK(touch_file("mock_sdcard/Roms/Has Games (ROM)/Actual Game-preview.mp4"),
          "create preview failed");
    CHECK(touch_file("mock_sdcard/Roms/Has Games (ROM)/Actual Game.sav"),
          "create save failed");

    CHECK(scan_console_dirs(false, &consoles, &count) == 0,
          "scan_console_dirs(false) failed");
    CHECK(count == 1, "expected one console, got %d", count);
    console = find_console_by_name(consoles, count, "Has Games (ROM)");
    CHECK(console != NULL, "did not find visible rom console");

    CHECK(scan_roms(console->path, false, &roms, &count) == 0,
          "scan_roms(false) failed");
    CHECK(count == 1, "expected one rom after filtering sidecars, got %d", count);
    rom = find_rom_by_display(roms, count, "Actual Game");
    CHECK(rom != NULL, "missing actual rom in rom scan");

    free(roms);
    roms = NULL;

    CHECK(scan_roms(console->path, true, &roms, &count) == 0,
          "scan_roms(true) failed");
    CHECK(count == 1,
          "expected one rom with show_hidden enabled, got %d", count);

    ok = true;

cleanup:
    free(roms);
    free(consoles);
    teardown_test_env(&env);
    return ok;
}

static bool test_nested_rom_console_qualifies(void)
{
    test_env env = {0};
    console_dir *consoles = NULL;
    rom_file *roms = NULL;
    int count = 0;
    bool ok = false;
    const console_dir *console = NULL;
    const rom_file *rom = NULL;

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(make_dir_recursive("mock_sdcard/Roms/Nested Games (NST)/Collections"),
          "mkdir nested fixture failed");
    CHECK(touch_file("mock_sdcard/Roms/Nested Games (NST)/Collections/Child Game.zip"),
          "create nested rom failed");

    CHECK(scan_console_dirs(false, &consoles, &count) == 0,
          "scan_console_dirs(false) failed");
    console = find_console_by_name(consoles, count, "Nested Games (NST)");
    CHECK(console != NULL, "nested console should be visible");

    CHECK(scan_roms(console->path, false, &roms, &count) == 0,
          "scan_roms(false) failed");
    CHECK(count == 1, "expected one nested rom, got %d", count);
    rom = find_rom_by_display(roms, count, "Child Game");
    CHECK(rom != NULL, "missing nested rom");
    CHECK(strstr(rom->path, "/Collections/Child Game.zip") != NULL,
          "nested rom path was not preserved: %s", rom->path);

    ok = true;

cleanup:
    free(roms);
    free(consoles);
    teardown_test_env(&env);
    return ok;
}

static bool test_empty_console_does_not_qualify(void)
{
    test_env env = {0};
    console_dir *consoles = NULL;
    int count = 0;
    bool ok = false;

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(make_dir_recursive("mock_sdcard/Roms/Empty Console (EMP)"),
          "mkdir empty console failed");

    CHECK(scan_console_dirs(false, &consoles, &count) == 0,
          "scan_console_dirs(false) failed");
    CHECK(count == 0, "expected empty console to stay hidden, got %d", count);
    free(consoles);
    consoles = NULL;

    CHECK(scan_console_dirs(true, &consoles, &count) == 0,
          "scan_console_dirs(true) failed");
    CHECK(count == 0,
          "expected empty console to stay hidden with show_hidden enabled, got %d",
          count);

    ok = true;

cleanup:
    free(consoles);
    teardown_test_env(&env);
    return ok;
}

static bool test_multidisc_and_cue_folders_survive(void)
{
    test_env env = {0};
    console_dir *consoles = NULL;
    rom_file *roms = NULL;
    int count = 0;
    bool ok = false;
    const console_dir *console = NULL;
    const rom_file *multi = NULL;
    const rom_file *cue = NULL;

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(make_dir_recursive("mock_sdcard/Roms/PlayStation (PS)/Final Fantasy VII"),
          "mkdir multidisc fixture failed");
    CHECK(make_dir_recursive("mock_sdcard/Roms/PlayStation (PS)/Formula 1 97"),
          "mkdir cue fixture failed");
    CHECK(write_file("mock_sdcard/Roms/PlayStation (PS)/Final Fantasy VII/Final Fantasy VII.m3u",
                     "disc1.chd\n"),
          "create m3u failed");
    CHECK(write_file("mock_sdcard/Roms/PlayStation (PS)/Formula 1 97/Formula 1 97.cue",
                     "FILE \"disc.bin\" BINARY\n"),
          "create cue failed");

    CHECK(scan_console_dirs(false, &consoles, &count) == 0,
          "scan_console_dirs(false) failed");
    console = find_console_by_name(consoles, count, "PlayStation (PS)");
    CHECK(console != NULL, "playstation console should be visible");

    CHECK(scan_roms(console->path, false, &roms, &count) == 0,
          "scan_roms(false) failed");
    CHECK(count == 2, "expected two disc-based entries, got %d", count);

    multi = find_rom_by_display(roms, count, "Final Fantasy VII");
    cue = find_rom_by_display(roms, count, "Formula 1 97");
    CHECK(multi != NULL && multi->is_multi_disc,
          "missing multidisc entry");
    CHECK(cue != NULL && cue->is_cue_folder,
          "missing cue-folder entry");

    ok = true;

cleanup:
    free(roms);
    free(consoles);
    teardown_test_env(&env);
    return ok;
}

static bool test_show_hidden_reveals_hidden_roms_not_sidecars(void)
{
    test_env env = {0};
    console_dir *consoles = NULL;
    rom_file *roms = NULL;
    int count = 0;
    bool ok = false;
    const console_dir *dreamcast = NULL;
    const console_dir *game_boy = NULL;
    const rom_file *tetris = NULL;

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(make_dir_recursive("mock_sdcard/Roms/.Dreamcast (DC)"),
          "mkdir hidden console failed");
    CHECK(make_dir_recursive("mock_sdcard/Roms/Game Boy (GB).disabled"),
          "mkdir disabled console failed");
    CHECK(touch_file("mock_sdcard/Roms/.Dreamcast (DC)/Power Stone.chd"),
          "create hidden rom failed");
    CHECK(touch_file("mock_sdcard/Roms/.Dreamcast (DC)/Power Stone-manual.pdf"),
          "create hidden manual failed");
    CHECK(touch_file("mock_sdcard/Roms/Game Boy (GB).disabled/Tetris.zip.disabled"),
          "create disabled rom failed");
    CHECK(touch_file("mock_sdcard/Roms/Game Boy (GB).disabled/Tetris-preview.mp4"),
          "create disabled preview failed");

    CHECK(scan_console_dirs(false, &consoles, &count) == 0,
          "scan_console_dirs(false) failed");
    CHECK(count == 0, "hidden consoles should stay hidden by default, got %d",
          count);
    free(consoles);
    consoles = NULL;

    CHECK(scan_console_dirs(true, &consoles, &count) == 0,
          "scan_console_dirs(true) failed");
    CHECK(count == 2, "expected two hidden consoles, got %d", count);
    dreamcast = find_console_by_name(consoles, count, ".Dreamcast (DC)");
    game_boy = find_console_by_name(consoles, count, "Game Boy (GB).disabled");
    CHECK(dreamcast != NULL, "missing tagged hidden console");
    CHECK(game_boy != NULL && game_boy->is_disabled,
          "missing disabled console");

    CHECK(scan_roms(dreamcast->path, true, &roms, &count) == 0,
          "scan_roms(hidden console) failed");
    CHECK(count == 1, "expected one hidden rom after filtering, got %d", count);
    CHECK(find_rom_by_display(roms, count, "Power Stone") != NULL,
          "missing hidden rom");
    free(roms);
    roms = NULL;

    CHECK(scan_roms(game_boy->path, true, &roms, &count) == 0,
          "scan_roms(disabled console) failed");
    CHECK(count == 1, "expected one disabled rom after filtering, got %d", count);
    tetris = find_rom_by_display(roms, count, "Tetris");
    CHECK(tetris != NULL && tetris->is_disabled,
          "missing disabled rom entry");

    ok = true;

cleanup:
    free(roms);
    free(consoles);
    teardown_test_env(&env);
    return ok;
}

static bool test_collection_console_labels_include_tags(void)
{
    test_env env = {0};
    console_dir *consoles = NULL;
    int count = 0;
    bool ok = false;
    const console_dir *ps = NULL;
    const console_dir *psp = NULL;
    const console_dir *md = NULL;

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(make_dir_recursive("mock_sdcard/Roms/Spelletjes (PS).disabled"),
          "mkdir ps collection failed");
    CHECK(make_dir_recursive("mock_sdcard/Roms/Spelletjes (PSP).disabled"),
          "mkdir psp collection failed");
    CHECK(make_dir_recursive("mock_sdcard/Roms/Spelletjes (MD).disabled"),
          "mkdir md collection failed");
    CHECK(touch_file("mock_sdcard/Roms/Spelletjes (PS).disabled/Spyro.chd"),
          "create ps rom failed");
    CHECK(touch_file("mock_sdcard/Roms/Spelletjes (PSP).disabled/LocoRoco.iso"),
          "create psp rom failed");
    CHECK(touch_file("mock_sdcard/Roms/Spelletjes (MD).disabled/Sonic.gen"),
          "create md rom failed");

    CHECK(scan_console_dirs(true, &consoles, &count) == 0,
          "scan_console_dirs(true) failed");
    CHECK(count == 3, "expected three tagged collections, got %d", count);

    ps = find_console_by_name(consoles, count, "Spelletjes (PS).disabled");
    psp = find_console_by_name(consoles, count, "Spelletjes (PSP).disabled");
    md = find_console_by_name(consoles, count, "Spelletjes (MD).disabled");
    CHECK(ps != NULL, "missing ps collection");
    CHECK(psp != NULL, "missing psp collection");
    CHECK(md != NULL, "missing md collection");
    CHECK(strcmp(ps->display, "Spelletjes (PS)") == 0,
          "unexpected ps display: %s", ps->display);
    CHECK(strcmp(psp->display, "Spelletjes (PSP)") == 0,
          "unexpected psp display: %s", psp->display);
    CHECK(strcmp(md->display, "Spelletjes (MD)") == 0,
          "unexpected md display: %s", md->display);

    ok = true;

cleanup:
    free(consoles);
    teardown_test_env(&env);
    return ok;
}

static bool test_map_txt_resolves_rom_titles(void)
{
    test_env env = {0};
    console_dir *consoles = NULL;
    rom_file *roms = NULL;
    int count = 0;
    bool ok = false;
    const console_dir *console = NULL;
    const rom_file *kof = NULL;
    const rom_file *samsho = NULL;
    const rom_file *multi = NULL;
    const rom_file *cue = NULL;
    const rom_file *unmapped = NULL;
    const rom_file *crend = NULL;
    static const char map_contents[] =
        "\xEF\xBB\xBF"
        "kof98.zip\tThe King of Fighters '98\n"
        "samsho.zip\tOld Samurai Title\n"
        "samsho.zip\tSamurai Shodown\n"
        "Metal Slug.m3u\tMetal Slug Collection\n"
        "Last Blade.cue\tThe Last Blade\n"
        "malformed line\n"
        "\tblank key\n"
        "blank value\t\n"
        "crend.zip\tCR End Title\r";

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(make_dir_recursive("mock_sdcard/Roms/Arcade (ARC)"),
          "mkdir arcade console failed");
    CHECK(make_dir_recursive("mock_sdcard/Roms/Arcade (ARC)/Metal Slug"),
          "mkdir multi fixture failed");
    CHECK(make_dir_recursive("mock_sdcard/Roms/Arcade (ARC)/Last Blade"),
          "mkdir cue fixture failed");
    CHECK(touch_file("mock_sdcard/Roms/Arcade (ARC)/kof98.zip"),
          "create kof rom failed");
    CHECK(touch_file("mock_sdcard/Roms/Arcade (ARC)/samsho.zip"),
          "create samsho rom failed");
    CHECK(touch_file("mock_sdcard/Roms/Arcade (ARC)/unmapped.zip"),
          "create unmapped rom failed");
    CHECK(touch_file("mock_sdcard/Roms/Arcade (ARC)/crend.zip"),
          "create crend rom failed");
    CHECK(write_file("mock_sdcard/Roms/Arcade (ARC)/Metal Slug/Metal Slug.m3u",
                     "disc1.chd\n"),
          "create multidisc companion failed");
    CHECK(write_file("mock_sdcard/Roms/Arcade (ARC)/Last Blade/Last Blade.cue",
                     "FILE \"disc.bin\" BINARY\n"),
          "create cue companion failed");
    CHECK(write_file("mock_sdcard/Roms/Arcade (ARC)/map.txt", map_contents),
          "write map.txt failed");

    CHECK(scan_console_dirs(false, &consoles, &count) == 0,
          "scan_console_dirs(false) failed");
    console = find_console_by_name(consoles, count, "Arcade (ARC)");
    CHECK(console != NULL, "missing arcade console");
    CHECK(strcmp(console->display, "Arcade (ARC)") == 0,
          "unexpected console display: %s", console->display);

    CHECK(scan_roms(console->path, false, &roms, &count) == 0,
          "scan_roms(false) failed");
    CHECK(count == 6, "expected six rom entries, got %d", count);

    kof = find_rom_by_name(roms, count, "kof98.zip");
    samsho = find_rom_by_name(roms, count, "samsho.zip");
    multi = find_rom_by_name(roms, count, "Metal Slug");
    cue = find_rom_by_name(roms, count, "Last Blade");
    unmapped = find_rom_by_name(roms, count, "unmapped.zip");
    crend = find_rom_by_name(roms, count, "crend.zip");
    CHECK(kof != NULL, "missing kof rom");
    CHECK(samsho != NULL, "missing samsho rom");
    CHECK(multi != NULL && multi->is_multi_disc, "missing mapped multidisc rom");
    CHECK(cue != NULL && cue->is_cue_folder, "missing mapped cue rom");
    CHECK(unmapped != NULL, "missing unmapped rom");
    CHECK(crend != NULL, "missing crend rom");
    CHECK(strcmp(crend->display, "CR End Title") == 0,
          "trailing CR not stripped from mapped title: %s", crend->display);

    CHECK(strcmp(kof->display, "The King of Fighters '98") == 0,
          "unexpected kof display: %s", kof->display);
    CHECK(strcmp(kof->source_stem, "kof98") == 0,
          "unexpected kof source stem: %s", kof->source_stem);
    CHECK(strcmp(samsho->display, "Samurai Shodown") == 0,
          "unexpected samsho display: %s", samsho->display);
    CHECK(strcmp(samsho->source_stem, "samsho") == 0,
          "unexpected samsho source stem: %s", samsho->source_stem);
    CHECK(strcmp(multi->display, "Metal Slug Collection") == 0,
          "unexpected multidisc display: %s", multi->display);
    CHECK(strcmp(multi->source_stem, "Metal Slug") == 0,
          "unexpected multidisc source stem: %s", multi->source_stem);
    CHECK(strcmp(cue->display, "The Last Blade") == 0,
          "unexpected cue display: %s", cue->display);
    CHECK(strcmp(cue->source_stem, "Last Blade") == 0,
          "unexpected cue source stem: %s", cue->source_stem);
    CHECK(strcmp(unmapped->display, "unmapped") == 0,
          "unexpected unmapped display: %s", unmapped->display);
    CHECK(strcmp(unmapped->source_stem, "unmapped") == 0,
          "unexpected unmapped source stem: %s", unmapped->source_stem);

    /* Malformed map.txt lines must not produce mappings. */
    for (int i = 0; i < count; i++) {
        CHECK(strcmp(roms[i].display, "malformed line") != 0,
              "malformed map line was accepted as a mapping");
        CHECK(strcmp(roms[i].display, "blank key") != 0,
              "empty-key map line was accepted as a mapping");
    }

    ok = true;

cleanup:
    free(roms);
    free(consoles);
    teardown_test_env(&env);
    return ok;
}

static bool test_mapped_shortcut_creation_preserves_raw_targets_and_artwork(void)
{
    test_env env = {0};
    console_dir *consoles = NULL;
    rom_file *roms = NULL;
    shortcut_entry *shortcuts = NULL;
    int count = 0;
    bool ok = false;
    const console_dir *console = NULL;
    const rom_file *rom = NULL;
    const shortcut_entry *shortcut = NULL;
    app_settings settings = {
        .copy_artwork = true,
        .artwork_mode = ART_MODE_BLACK,
        .show_hidden = true,
    };
    char marker_path[SC_MAX_PATH * 2];
    char m3u_path[SC_MAX_PATH * 2];
    char root_thumb_path[SC_MAX_PATH * 2];
    static const char map_contents[] =
        "kof98.zip\tThe King of Fighters '98\n";

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(make_dir_recursive("mock_sdcard/Roms/Arcade (ARC).disabled/.media"),
          "mkdir disabled arcade console failed");
    CHECK(touch_file("mock_sdcard/Roms/Arcade (ARC).disabled/kof98.zip.disabled"),
          "create disabled mapped rom failed");
    CHECK(write_file("mock_sdcard/Roms/Arcade (ARC).disabled/.media/kof98.png",
                     "PNGDATA"),
          "create disabled mapped source art failed");
    CHECK(write_file("mock_sdcard/Roms/Arcade (ARC).disabled/map.txt",
                     map_contents),
          "write disabled map.txt failed");

    CHECK(scan_console_dirs(true, &consoles, &count) == 0,
          "scan_console_dirs(true) failed");
    console = find_console_by_name(consoles, count, "Arcade (ARC).disabled");
    CHECK(console != NULL, "missing disabled arcade console");
    CHECK(strcmp(console->display, "Arcade (ARC)") == 0,
          "unexpected disabled console display: %s", console->display);

    CHECK(scan_roms(console->path, true, &roms, &count) == 0,
          "scan_roms(true) failed");
    CHECK(count == 1, "expected one disabled mapped rom, got %d", count);
    rom = find_rom_by_name(roms, count, "kof98.zip.disabled");
    CHECK(rom != NULL, "missing disabled mapped rom");
    CHECK(rom->is_disabled, "mapped rom should be marked disabled");
    CHECK(strcmp(rom->display, "The King of Fighters '98") == 0,
          "unexpected mapped display: %s", rom->display);
    CHECK(strcmp(rom->source_stem, "kof98") == 0,
          "unexpected mapped source stem: %s", rom->source_stem);

    reset_generate_artwork_bg_stub();
    CHECK(create_rom_shortcut(rom->display, console->tag, rom,
                              SC_POS_ALPHA, &settings) == 0,
          "create_rom_shortcut failed for mapped rom");

    CHECK(scan_shortcuts(&shortcuts, &count) == 0,
          "scan_shortcuts failed");
    shortcut = find_shortcut_by_display(shortcuts, count,
                                        "The King of Fighters '98");
    CHECK(shortcut != NULL, "missing created mapped shortcut");
    CHECK(snprintf(marker_path, sizeof(marker_path), "%s/.shortcut",
                   shortcut->path) < (int)sizeof(marker_path),
          "mapped marker path too long");
    CHECK(snprintf(m3u_path, sizeof(m3u_path), "%s/%s.m3u",
                   shortcut->path, shortcut->name) < (int)sizeof(m3u_path),
          "mapped m3u path too long");
    CHECK(file_contents_equal(marker_path, "The King of Fighters '98"),
          "shortcut marker did not use mapped title");
    CHECK(file_contents_equal(m3u_path,
                              "../Arcade (ARC).disabled/kof98.zip.disabled"),
          "shortcut m3u did not preserve raw rom target");
    CHECK(g_generate_artwork_bg_call_count == 1,
          "expected one artwork generation call, got %d",
          g_generate_artwork_bg_call_count);
    CHECK(ends_with(g_last_generated_art_src_path,
                    "/mock_sdcard/Roms/Arcade (ARC).disabled/.media/kof98.png"),
          "unexpected art src path: %s", g_last_generated_art_src_path);
    CHECK(strstr(g_last_generated_art_dest_folder, shortcut->name) != NULL,
          "unexpected art dest folder: %s", g_last_generated_art_dest_folder);
    CHECK(snprintf(root_thumb_path, sizeof(root_thumb_path),
                   "mock_sdcard/Roms/.media/%s.png", shortcut->name) <
          (int)sizeof(root_thumb_path),
          "mapped root thumbnail path too long");
    CHECK(file_contents_equal(root_thumb_path, "PNGDATA"),
          "mapped root thumbnail was not copied from source art");
    reset_generate_artwork_bg_stub();
    CHECK(regenerate_all_media(&settings) == 0,
          "regenerate_all_media failed for disabled mapped rom");
    CHECK(g_generate_artwork_bg_call_count == 1,
          "expected one regenerated artwork call, got %d",
          g_generate_artwork_bg_call_count);
    CHECK(ends_with(g_last_generated_art_src_path,
                    "/mock_sdcard/Roms/Arcade (ARC).disabled/.media/kof98.png"),
          "unexpected regenerated disabled art src path: %s",
          g_last_generated_art_src_path);
    CHECK(file_contents_equal(root_thumb_path, "PNGDATA"),
          "regenerate removed disabled mapped root thumbnail");

    ok = true;

cleanup:
    free(shortcuts);
    free(roms);
    free(consoles);
    teardown_test_env(&env);
    return ok;
}

static bool test_regenerate_disabled_multidisc_uses_console_artwork(void)
{
    test_env env = {0};
    console_dir *consoles = NULL;
    rom_file *roms = NULL;
    shortcut_entry *shortcuts = NULL;
    int count = 0;
    bool ok = false;
    const console_dir *console = NULL;
    const rom_file *rom = NULL;
    const shortcut_entry *shortcut = NULL;
    app_settings settings = {
        .copy_artwork = true,
        .artwork_mode = ART_MODE_BLACK,
        .show_hidden = true,
    };
    char root_thumb_path[SC_MAX_PATH * 2];

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(make_dir_recursive("mock_sdcard/Roms/PlayStation (PS).disabled/.media"),
          "mkdir disabled playstation media failed");
    CHECK(make_dir_recursive("mock_sdcard/Roms/PlayStation (PS).disabled/Final Fantasy VII.disabled"),
          "mkdir disabled multidisc fixture failed");
    CHECK(write_file("mock_sdcard/Roms/PlayStation (PS).disabled/Final Fantasy VII.disabled/Final Fantasy VII.m3u",
                     "disc1.chd\n"),
          "create disabled multidisc companion failed");
    CHECK(write_file("mock_sdcard/Roms/PlayStation (PS).disabled/.media/Final Fantasy VII.png",
                     "MULTIART"),
          "create disabled multidisc source art failed");

    CHECK(scan_console_dirs(true, &consoles, &count) == 0,
          "scan_console_dirs(true) failed");
    console = find_console_by_name(consoles, count,
                                   "PlayStation (PS).disabled");
    CHECK(console != NULL, "missing disabled playstation console");

    CHECK(scan_roms(console->path, true, &roms, &count) == 0,
          "scan_roms(true) failed");
    rom = find_rom_by_name(roms, count, "Final Fantasy VII.disabled");
    CHECK(rom != NULL && rom->is_multi_disc,
          "missing disabled multidisc rom");
    CHECK(strcmp(rom->source_stem, "Final Fantasy VII") == 0,
          "unexpected disabled multidisc source stem: %s",
          rom->source_stem);

    reset_generate_artwork_bg_stub();
    CHECK(create_rom_shortcut(rom->display, console->tag, rom,
                              SC_POS_ALPHA, &settings) == 0,
          "create_rom_shortcut failed for disabled multidisc rom");

    CHECK(scan_shortcuts(&shortcuts, &count) == 0,
          "scan_shortcuts failed");
    shortcut = find_shortcut_by_display(shortcuts, count, "Final Fantasy VII");
    CHECK(shortcut != NULL, "missing disabled multidisc shortcut");
    CHECK(snprintf(root_thumb_path, sizeof(root_thumb_path),
                   "mock_sdcard/Roms/.media/%s.png", shortcut->name) <
          (int)sizeof(root_thumb_path),
          "disabled multidisc root thumbnail path too long");
    CHECK(file_contents_equal(root_thumb_path, "MULTIART"),
          "disabled multidisc root thumbnail was not copied from source art");

    reset_generate_artwork_bg_stub();
    CHECK(regenerate_all_media(&settings) == 0,
          "regenerate_all_media failed for disabled multidisc rom");
    CHECK(g_generate_artwork_bg_call_count == 1,
          "expected one regenerated multidisc artwork call, got %d",
          g_generate_artwork_bg_call_count);
    CHECK(ends_with(g_last_generated_art_src_path,
                    "/mock_sdcard/Roms/PlayStation (PS).disabled/.media/Final Fantasy VII.png"),
          "unexpected regenerated disabled multidisc art src path: %s",
          g_last_generated_art_src_path);
    CHECK(file_contents_equal(root_thumb_path, "MULTIART"),
          "regenerate removed disabled multidisc root thumbnail");

    ok = true;

cleanup:
    free(shortcuts);
    free(roms);
    free(consoles);
    teardown_test_env(&env);
    return ok;
}

static bool test_slash_mapped_shortcut_creation_uses_storage_safe_names(void)
{
    test_env env = {0};
    console_dir *consoles = NULL;
    rom_file *roms = NULL;
    shortcut_entry *shortcuts = NULL;
    int count = 0;
    bool ok = false;
    const console_dir *console = NULL;
    const rom_file *rom = NULL;
    const shortcut_entry *shortcut = NULL;
    app_settings settings = {
        .copy_artwork = true,
        .artwork_mode = ART_MODE_BLACK,
        .show_hidden = true,
    };
    char marker_path[SC_MAX_PATH * 2];
    char m3u_path[SC_MAX_PATH * 2];
    char root_thumb_path[SC_MAX_PATH * 2];
    static const char mapped_title[] = "Aero Fighters 3 / Sonic Wings 3";
    static const char map_contents[] =
        "sonicwi3.zip\tAero Fighters 3 / Sonic Wings 3\n";

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(make_dir_recursive("mock_sdcard/Roms/Arcade (FBN)/.media"),
          "mkdir arcade console failed");
    CHECK(touch_file("mock_sdcard/Roms/Arcade (FBN)/sonicwi3.zip"),
          "create mapped rom failed");
    CHECK(write_file("mock_sdcard/Roms/Arcade (FBN)/.media/sonicwi3.png",
                     "SLASHART"),
          "create slash-mapped source art failed");
    CHECK(write_file("mock_sdcard/Roms/Arcade (FBN)/map.txt", map_contents),
          "write slash map.txt failed");

    CHECK(scan_console_dirs(true, &consoles, &count) == 0,
          "scan_console_dirs(true) failed");
    console = find_console_by_name(consoles, count, "Arcade (FBN)");
    CHECK(console != NULL, "missing arcade fbn console");

    CHECK(scan_roms(console->path, true, &roms, &count) == 0,
          "scan_roms(true) failed");
    rom = find_rom_by_name(roms, count, "sonicwi3.zip");
    CHECK(rom != NULL, "missing slash-mapped rom");
    CHECK(strcmp(rom->display, mapped_title) == 0,
          "unexpected slash-mapped display: %s", rom->display);
    CHECK(strcmp(rom->source_stem, "sonicwi3") == 0,
          "unexpected slash-mapped source stem: %s", rom->source_stem);

    reset_generate_artwork_bg_stub();
    CHECK(create_rom_shortcut(rom->display, console->tag, rom,
                              SC_POS_ALPHA, &settings) == 0,
          "create_rom_shortcut failed for slash-mapped rom");

    CHECK(scan_shortcuts(&shortcuts, &count) == 0,
          "scan_shortcuts failed");
    shortcut = find_shortcut_by_display(shortcuts, count, mapped_title);
    CHECK(shortcut != NULL, "missing created slash-mapped shortcut");
    CHECK(strstr(shortcut->name, "%2F") != NULL,
          "shortcut storage name should encode slash: %s", shortcut->name);
    CHECK(snprintf(marker_path, sizeof(marker_path), "%s/.shortcut",
                   shortcut->path) < (int)sizeof(marker_path),
          "marker path too long");
    CHECK(snprintf(m3u_path, sizeof(m3u_path), "%s/%s.m3u",
                   shortcut->path, shortcut->name) < (int)sizeof(m3u_path),
          "m3u path too long");
    CHECK(file_contents_equal(marker_path, mapped_title),
          "shortcut marker did not preserve slash title");
    CHECK(file_contents_equal(m3u_path, "../Arcade (FBN)/sonicwi3.zip"),
          "slash-mapped shortcut m3u did not preserve raw target");
    CHECK(ends_with(shortcut->target_path,
                    "/mock_sdcard/Roms/Arcade (FBN)/sonicwi3.zip"),
          "unexpected slash-mapped shortcut target: %s", shortcut->target_path);
    CHECK(g_generate_artwork_bg_call_count == 1,
          "expected one artwork generation call, got %d",
          g_generate_artwork_bg_call_count);
    CHECK(ends_with(g_last_generated_art_src_path,
                    "/mock_sdcard/Roms/Arcade (FBN)/.media/sonicwi3.png"),
          "unexpected slash-mapped art src path: %s",
          g_last_generated_art_src_path);
    CHECK(snprintf(root_thumb_path, sizeof(root_thumb_path),
                   "mock_sdcard/Roms/.media/%s.png", shortcut->name) <
          (int)sizeof(root_thumb_path),
          "slash-mapped root thumbnail path too long");
    CHECK(file_contents_equal(root_thumb_path, "SLASHART"),
          "slash-mapped root thumbnail was not copied from source art");

    ok = true;

cleanup:
    free(shortcuts);
    free(roms);
    free(consoles);
    teardown_test_env(&env);
    return ok;
}

static bool test_ports_dotports_always_hidden(void)
{
    test_env env = {0};
    console_dir *consoles = NULL;
    rom_file *roms = NULL;
    int count = 0;
    bool ok = false;
    const console_dir *ports = NULL;

    CHECK(setup_test_env(&env), "setup failed");

    /* Top-level launch scripts (the actual ROM entries). */
    CHECK(make_dir_recursive("mock_sdcard/Roms/Ports (PORTS)"),
          "mkdir ports console failed");
    CHECK(touch_file("mock_sdcard/Roms/Ports (PORTS)/0) Portmaster.sh"),
          "create portmaster failed");
    CHECK(touch_file("mock_sdcard/Roms/Ports (PORTS)/PokeMMO.sh"),
          "create pokemmo launcher failed");

    /* .ports internal data — should never appear regardless of show_hidden. */
    CHECK(make_dir_recursive("mock_sdcard/Roms/Ports (PORTS)/.ports/pokemmo/src/com"),
          "mkdir ports data tree failed");
    CHECK(make_dir_recursive("mock_sdcard/Roms/Ports (PORTS)/.ports/pokemmo/roms"),
          "mkdir ports roms dir failed");
    CHECK(touch_file("mock_sdcard/Roms/Ports (PORTS)/.ports/PokeMMO.sh"),
          "create ports internal sh failed");
    CHECK(touch_file("mock_sdcard/Roms/Ports (PORTS)/.ports/pokemmo/port.json"),
          "create port.json failed");
    CHECK(touch_file("mock_sdcard/Roms/Ports (PORTS)/.ports/pokemmo/RELEASE"),
          "create RELEASE failed");
    CHECK(touch_file("mock_sdcard/Roms/Ports (PORTS)/.ports/pokemmo/src/HO.java"),
          "create java source failed");
    CHECK(touch_file("mock_sdcard/Roms/Ports (PORTS)/.ports/pokemmo/src/com/Foo.class"),
          "create class file failed");
    CHECK(touch_file("mock_sdcard/Roms/Ports (PORTS)/.ports/pokemmo/roms/.gitkeep"),
          "create gitkeep failed");

    CHECK(scan_console_dirs(false, &consoles, &count) == 0,
          "scan_console_dirs(false) failed");
    ports = find_console_by_name(consoles, count, "Ports (PORTS)");
    CHECK(ports != NULL, "ports console should be visible");

    /* With show_hidden=true, .ports must still be excluded. */
    CHECK(scan_roms(ports->path, true, &roms, &count) == 0,
          "scan_roms(true) failed");
    CHECK(count == 2,
          "expected 2 top-level launch scripts, got %d", count);
    CHECK(find_rom_by_display(roms, count, "0) Portmaster") != NULL,
          "missing Portmaster entry");
    CHECK(find_rom_by_display(roms, count, "PokeMMO") != NULL,
          "missing PokeMMO entry");

    ok = true;

cleanup:
    free(roms);
    free(consoles);
    teardown_test_env(&env);
    return ok;
}

static bool test_tool_names_preserve_internal_pak_suffixes(void)
{
    test_env env = {0};
    tool_pak *tools = NULL;
    int count = 0;
    bool ok = false;
    const tool_pak *tool = NULL;
    const tool_pak *disabled_tool = NULL;

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(make_dir_recursive("mock_sdcard/Tools/tg5040/My.pak Helper.pak"),
          "mkdir tool fixture failed");
    CHECK(make_dir_recursive("mock_sdcard/Tools/tg5040/Disabled Tool.pak.disabled"),
          "mkdir disabled tool fixture failed");

    CHECK(scan_tools(false, &tools, &count) == 0,
          "scan_tools(false) failed");
    CHECK(count == 1, "expected one visible tool, got %d", count);
    tool = find_tool_by_name(tools, count, "My.pak Helper");
    CHECK(tool != NULL, "missing tool with internal .pak in name");
    CHECK(strcmp(tool->display, "My.pak Helper") == 0,
          "unexpected visible tool display: %s", tool->display);
    free(tools);
    tools = NULL;

    CHECK(scan_tools(true, &tools, &count) == 0,
          "scan_tools(true) failed");
    CHECK(count == 2, "expected two tools with show_hidden enabled, got %d",
          count);
    disabled_tool = find_tool_by_name(tools, count, "Disabled Tool");
    CHECK(disabled_tool != NULL, "missing disabled tool");
    CHECK(strcmp(disabled_tool->display, "Disabled Tool  [disabled]") == 0,
          "unexpected disabled tool display: %s", disabled_tool->display);

    ok = true;

cleanup:
    free(tools);
    teardown_test_env(&env);
    return ok;
}

static bool test_tool_shortcut_m3u_uses_valid_relative_tool_path(void)
{
    test_env env = {0};
    app_settings settings = { .copy_artwork = false };
    bool ok = false;
    char pak_path[SC_MAX_PATH];
    char m3u_path[SC_MAX_PATH];
    char target_path[SC_MAX_PATH];

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(make_dir_recursive("mock_sdcard/Tools/tg5040/Foo.pak"),
          "mkdir tool pak failed");
    CHECK(build_absolute_path(&env, "mock_sdcard/Tools/tg5040/Foo.pak",
                              pak_path, sizeof(pak_path)),
          "tool pak path too long");

    CHECK(create_tool_shortcut("Foo", pak_path, SC_POS_ALPHA, &settings) == 0,
          "create_tool_shortcut failed");
    CHECK(snprintf(m3u_path, sizeof(m3u_path),
                   "mock_sdcard/Roms/Foo (SHORTCUT)/Foo (SHORTCUT).m3u") <
          (int)sizeof(m3u_path),
          "m3u path too long");
    CHECK(snprintf(target_path, sizeof(target_path),
                   "mock_sdcard/Roms/Foo (SHORTCUT)/target") <
          (int)sizeof(target_path),
          "target path too long");
    CHECK(file_contents_equal(m3u_path, "../../Tools/tg5040/Foo.pak"),
          "tool shortcut m3u content mismatch");
    CHECK(file_contents_equal(target_path, pak_path),
          "tool shortcut target file mismatch");

    ok = true;

cleanup:
    teardown_test_env(&env);
    return ok;
}

static bool test_tool_shortcut_rejects_pak_outside_sd_root(void)
{
    test_env env = {0};
    app_settings settings = { .copy_artwork = false };
    bool ok = false;
    char pak_path[SC_MAX_PATH];

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(build_absolute_path(&env, "outside/Foo.pak",
                              pak_path, sizeof(pak_path)),
          "outside pak path too long");
    CHECK(create_tool_shortcut("Foo", pak_path, SC_POS_ALPHA, &settings) != 0,
          "create_tool_shortcut should reject outside pak path");
    CHECK(!path_exists("mock_sdcard/Roms/Foo (SHORTCUT)"),
          "invalid tool shortcut should not create folder");

    ok = true;

cleanup:
    teardown_test_env(&env);
    return ok;
}

static bool bridge_script_accepts_file_and_dir_targets(const char *script_path)
{
    char cwd[SC_MAX_PATH];
    char target_dir[SC_MAX_PATH];
    char shortcut_arg[SC_MAX_PATH];
    char launch_path[SC_MAX_PATH];
    char tool_path[SC_MAX_PATH];
    char launch_script[SC_MAX_PATH * 2];
    char tool_script[SC_MAX_PATH * 2];
    char target_file[SC_MAX_PATH];
    char marker_path[SC_MAX_PATH];
    char cmd[SC_MAX_PATH * 4];

    if (!getcwd(cwd, sizeof(cwd)))
        return false;
    if (snprintf(marker_path, sizeof(marker_path), "%s/bridge-marker.txt",
                 cwd) >= (int)sizeof(marker_path))
        return false;
    if (snprintf(target_dir, sizeof(target_dir),
                 "mock_sdcard/Tools/tg5040/Foo.pak") >= (int)sizeof(target_dir))
        return false;
    if (snprintf(shortcut_arg, sizeof(shortcut_arg),
                 "mock_sdcard/Roms/Foo (SHORTCUT)/../../Tools/tg5040/Foo.pak") >=
        (int)sizeof(shortcut_arg))
        return false;
    if (snprintf(launch_path, sizeof(launch_path), "%s/launch.sh",
                 target_dir) >= (int)sizeof(launch_path))
        return false;
    if (snprintf(tool_path, sizeof(tool_path), "%s/tool.sh",
                 target_dir) >= (int)sizeof(tool_path))
        return false;
    if (!make_dir_recursive(target_dir) ||
        !make_dir_recursive("mock_sdcard/Roms/Foo (SHORTCUT)"))
        return false;
    if (snprintf(launch_script, sizeof(launch_script),
                 "#!/bin/sh\ncd $(dirname \"$0\")\n./tool.sh\n") >=
        (int)sizeof(launch_script))
        return false;
    if (snprintf(tool_script, sizeof(tool_script),
                 "#!/bin/sh\nprintf 'ran\\n' >> '%s'\n",
                 marker_path) >= (int)sizeof(tool_script))
        return false;
    if (!write_executable_file(launch_path, launch_script))
        return false;
    if (!write_executable_file(tool_path, tool_script))
        return false;
    if (snprintf(target_file, sizeof(target_file), "bridge-target.txt") >=
        (int)sizeof(target_file))
        return false;
    if (!write_file(target_file, target_dir))
        return false;

    if (snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\"",
                 script_path, target_file) >= (int)sizeof(cmd))
        return false;
    if (!run_command_success(cmd))
        return false;

    if (snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\"",
                 script_path, shortcut_arg) >= (int)sizeof(cmd))
        return false;
    if (!run_command_success(cmd))
        return false;

    return file_contents_equal(marker_path, "ran\nran\n");
}

static bool test_bridge_launcher_accepts_file_and_dir_targets(void)
{
    test_env env = {0};
    bool ok = false;
    char embedded_path[SC_MAX_PATH];
    char resource_path[SC_MAX_PATH];

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(snprintf(embedded_path, sizeof(embedded_path),
                   "embedded-bridge.sh") < (int)sizeof(embedded_path),
          "embedded bridge path too long");
    CHECK(write_executable_file(embedded_path,
                                bridge_launch_script_for_tests()),
          "write embedded bridge script failed");
    CHECK(bridge_script_accepts_file_and_dir_targets("./embedded-bridge.sh"),
          "embedded bridge script did not accept both target formats");

    CHECK(remove_tree("mock_sdcard"), "reset bridge fixture failed");
    CHECK(remove_tree("bridge-marker.txt"), "reset bridge marker failed");
    CHECK(remove_tree("bridge-target.txt"), "reset bridge target failed");
    CHECK(make_dir_recursive("mock_sdcard/Roms") &&
          make_dir_recursive("mock_sdcard/.userdata/shared") &&
          make_dir_recursive("mock_sdcard/.userdata/tg5040/logs"),
          "recreate test env dirs failed");
    CHECK(snprintf(resource_path, sizeof(resource_path),
                   "%s/resources/SHORTCUT.pak/launch.sh",
                   env.original_cwd) < (int)sizeof(resource_path),
          "resource bridge path too long");
    CHECK(bridge_script_accepts_file_and_dir_targets(resource_path),
          "resource bridge script did not accept both target formats");

    ok = true;

cleanup:
    teardown_test_env(&env);
    return ok;
}

static bool test_symlinked_entries_are_skipped(void)
{
    test_env env = {0};
    console_dir *consoles = NULL;
    tool_pak *tools = NULL;
    shortcut_entry *shortcuts = NULL;
    int count = 0;
    bool ok = false;
    char console_target[SC_MAX_PATH];
    char tool_target[SC_MAX_PATH];
    char shortcut_target[SC_MAX_PATH];

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(make_dir_recursive("mock_sdcard/Tools/tg5040"),
          "mkdir tools root failed");
    CHECK(make_dir_recursive("outside/Linked Console (LNK)"),
          "mkdir external console failed");
    CHECK(touch_file("outside/Linked Console (LNK)/Game.zip"),
          "create external rom failed");
    CHECK(make_dir_recursive("outside/Linked Tool.pak"),
          "mkdir external tool failed");
    CHECK(make_dir_recursive("outside/Linked Shortcut (TAG)"),
          "mkdir external shortcut failed");
    CHECK(write_file("outside/Linked Shortcut (TAG)/.shortcut",
                     "Linked Shortcut"),
          "write external shortcut marker failed");
    CHECK(write_file("outside/Linked Shortcut (TAG)/Linked Shortcut (TAG).m3u",
                     "../Console (TAG)/Game.zip"),
          "write external shortcut m3u failed");

    CHECK(snprintf(console_target, sizeof(console_target), "%s/outside/Linked Console (LNK)",
                   env.temp_root) < (int)sizeof(console_target),
          "console target path too long");
    CHECK(snprintf(tool_target, sizeof(tool_target), "%s/outside/Linked Tool.pak",
                   env.temp_root) < (int)sizeof(tool_target),
          "tool target path too long");
    CHECK(snprintf(shortcut_target, sizeof(shortcut_target),
                   "%s/outside/Linked Shortcut (TAG)", env.temp_root) <
          (int)sizeof(shortcut_target),
          "shortcut target path too long");

    CHECK(symlink(console_target,
                  "mock_sdcard/Roms/Linked Console (LNK)") == 0,
          "create console symlink failed");
    CHECK(symlink(tool_target,
                  "mock_sdcard/Tools/tg5040/Linked Tool.pak") == 0,
          "create tool symlink failed");
    CHECK(symlink(shortcut_target,
                  "mock_sdcard/Roms/Linked Shortcut (TAG)") == 0,
          "create shortcut symlink failed");

    CHECK(scan_console_dirs(false, &consoles, &count) == 0,
          "scan_console_dirs(false) failed");
    CHECK(count == 0, "expected symlinked console to be skipped, got %d",
          count);
    free(consoles);
    consoles = NULL;

    CHECK(scan_tools(false, &tools, &count) == 0,
          "scan_tools(false) failed");
    CHECK(count == 0, "expected symlinked tool to be skipped, got %d", count);
    free(tools);
    tools = NULL;

    CHECK(scan_shortcuts(&shortcuts, &count) == 0,
          "scan_shortcuts failed");
    CHECK(count == 0, "expected symlinked shortcut to be skipped, got %d",
          count);

    ok = true;

cleanup:
    free(shortcuts);
    free(tools);
    free(consoles);
    teardown_test_env(&env);
    return ok;
}

static bool test_ensure_dir_exists_rejects_file_collisions(void)
{
    test_env env = {0};
    bool ok = false;

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(write_file("mock_sdcard/Roms/existing-file", "not a directory"),
          "write final collision file failed");
    CHECK(write_file("mock_sdcard/blocker", "not a directory"),
          "write intermediate collision file failed");

    CHECK(ensure_dir_exists("mock_sdcard/Roms/existing-file") != 0,
          "expected existing file at final path to fail");
    CHECK(ensure_dir_exists("mock_sdcard/blocker/child") != 0,
          "expected existing file in parent path to fail");

    ok = true;

cleanup:
    teardown_test_env(&env);
    return ok;
}

static bool test_resume_hook_install_is_idempotent(void)
{
    static const char expected_script[] =
        "#!/bin/sh\n"
        "\n"
        "if [ \"${HOOK_PHASE:-}\" != \"post\" ]; then\n"
        "    exit 0\n"
        "fi\n"
        "case \"${HOOK_TYPE:-}\" in\n"
        "    rom|pak|tool) ;;\n"
        "    *) exit 0 ;;\n"
        "esac\n"
        "\n"
        "HELPER=\"${SDCARD_PATH:-/mnt/SDCARD}/Tools/${PLATFORM:-tg5040}/Shortcuts.pak/shortcuts\"\n"
        "[ -x \"$HELPER\" ] || exit 0\n"
        "\n"
        "LD_LIBRARY_PATH=\"${LD_LIBRARY_PATH:-${SDCARD_PATH:-/mnt/SDCARD}/.system/${PLATFORM:-tg5040}/lib}\"\n"
        "export LD_LIBRARY_PATH\n"
        "\n"
        "LOG_DIR=\"${LOGS_PATH:-${USERDATA_PATH:-${SDCARD_PATH:-/mnt/SDCARD}/.userdata/${PLATFORM:-tg5040}}/logs}\"\n"
        "mkdir -p \"$LOG_DIR\"\n"
        "cd \"${SDCARD_PATH:-/mnt/SDCARD}/Tools/${PLATFORM:-tg5040}/Shortcuts.pak\" || exit 0\n"
        "\"./shortcuts\" --resume-sync-hook >>\"$LOG_DIR/shortcuts-resume-sync.txt\" 2>&1\n";

    test_env env = {0};
    char hook_path[SC_MAX_PATH];
    struct stat st;
    bool ok = false;

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(build_resume_hook_path(hook_path, sizeof(hook_path)),
          "hook path too long");

    CHECK(ensure_resume_hook_installed() == 0,
          "first hook install failed");
    CHECK(path_exists(hook_path), "hook script missing after first install");
    CHECK(file_contents_equal(hook_path, expected_script),
          "hook script content mismatch after first install");
    CHECK(stat(hook_path, &st) == 0, "stat failed for hook script");
    CHECK((st.st_mode & S_IXUSR) != 0, "hook script is not executable");

    CHECK(ensure_resume_hook_installed() == 0,
          "second hook install failed");
    CHECK(file_contents_equal(hook_path, expected_script),
          "hook script content mismatch after second install");

    ok = true;

cleanup:
    teardown_test_env(&env);
    return ok;
}

static bool test_resume_sync_hook_noops_for_pak_launches(void)
{
    test_env env = {0};
    bool ok = false;
    char manifest_path[SC_MAX_PATH];

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(build_resume_manifest_path(manifest_path, sizeof(manifest_path)),
          "manifest path too long");
    CHECK(set_hook_env("pak", "mock_sdcard/Roms/Foo (BAR)"),
          "set hook env failed");

    CHECK(resume_sync_from_hook_env() == 0,
          "hook sync for pak launch failed");
    CHECK(!path_exists(manifest_path),
          "manifest should not exist after pak launch");

    ok = true;

cleanup:
    clear_hook_env();
    teardown_test_env(&env);
    return ok;
}

static bool test_pak_launch_dedups_recent_without_trailing_newline(void)
{
    test_env env = {0};
    bool ok = false;
    const char *recent_path = "mock_sdcard/.userdata/shared/.minui/recent.txt";

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(make_dir_recursive("mock_sdcard/.userdata/shared/.minui"),
          "mkdir recent dir failed");
    CHECK(write_file(recent_path,
                     "/Roms/GBA (GBA)/Metroid Fusion.gba\tMetroid Fusion\n"
                     "/Tools/tg5040/Foo.pak\tFoo"),
          "write recent fixture failed");
    CHECK(set_hook_env("pak", "/mnt/SDCARD/Tools/tg5040/Foo.pak"),
          "set pak hook env failed");

    CHECK(resume_sync_from_hook_env() == 0,
          "pak hook sync failed");
    CHECK(file_contents_equal(recent_path,
                              "/Roms/GBA (GBA)/Metroid Fusion.gba\tMetroid Fusion\n"),
          "recent.txt was not deduped for final line without newline");

    ok = true;

cleanup:
    clear_hook_env();
    teardown_test_env(&env);
    return ok;
}

static bool test_pak_launch_prunes_game_tracker_tool_shortcuts(void)
{
    test_env env = {0};
    bool ok = false;
    const char *db_path = "mock_sdcard/.userdata/shared/game_logs.sqlite";
    int count = 0;

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(make_dir_recursive("mock_sdcard/.userdata/shared"),
          "mkdir shared userdata failed");
    CHECK(sqlite_exec_sql(db_path,
          "CREATE TABLE rom("
          "id INTEGER PRIMARY KEY, type TEXT, name TEXT, file_path TEXT, "
          "image_path TEXT, created_at INTEGER, updated_at INTEGER);"
          "CREATE TABLE play_activity("
          "rom_id INTEGER, play_time INTEGER, created_at INTEGER, "
          "updated_at INTEGER);"
          "INSERT INTO rom(id, name, file_path) VALUES"
          "(1, 'Sonic', 'Sega Genesis (MD)/Sonic.zip'),"
          "(2, 'Central Scrutinizer', '../Tools/tg5040/Central Scrutinizer.pak'),"
          "(3, 'Updater', '/mnt/SDCARD/Tools/tg5040/Updater.pak'),"
          "(4, 'Battery', 'Tools/tg5040/Battery.pak'),"
          "(5, 'Legacy Tool', 'Legacy (SHORTCUT)/../../Tools/tg5040/Legacy.pak'),"
          "(6, '0) Portmaster', 'Ports (PORTS)/0) Portmaster.sh');"
          "INSERT INTO play_activity(rom_id, play_time) VALUES"
          "(1, 30), (2, 40), (3, 50), (4, 60), (5, 70), (6, 80);"),
          "create game tracker fixture failed");
    CHECK(set_hook_env("pak", "/mnt/SDCARD/Tools/tg5040/Updater.pak"),
          "set pak hook env failed");

    CHECK(resume_sync_from_hook_env() == 0,
          "pak hook sync failed");
    CHECK(sqlite_query_int(db_path, "SELECT COUNT(*) FROM rom;", &count),
          "count rom rows failed");
    CHECK(count == 2, "expected 2 remaining rom rows, got %d", count);
    CHECK(sqlite_query_int(db_path, "SELECT COUNT(*) FROM play_activity;", &count),
          "count play_activity rows failed");
    CHECK(count == 2, "expected 2 remaining play_activity rows, got %d", count);
    CHECK(sqlite_query_int(db_path,
          "SELECT COUNT(*) FROM rom WHERE file_path LIKE '%Tools/%';",
          &count), "count tool tracker rows failed");
    CHECK(count == 0, "expected no remaining tool tracker rows, got %d", count);
    CHECK(sqlite_query_int(db_path,
          "SELECT COUNT(*) FROM rom WHERE file_path LIKE 'Ports (PORTS)/%';",
          &count), "count port rows failed");
    CHECK(count == 1, "expected Port row to remain, got %d", count);

    ok = true;

cleanup:
    clear_hook_env();
    teardown_test_env(&env);
    return ok;
}

static bool test_resume_sync_hook_noops_for_direct_roms_and_non_shortcuts(void)
{
    test_env env = {0};
    bool ok = false;
    char direct_rom_abs[SC_MAX_PATH];
    char plain_folder_abs[SC_MAX_PATH];
    char manifest_path[SC_MAX_PATH];

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(make_dir_recursive("mock_sdcard/Roms/Game Boy Advance (GBA)"),
          "mkdir rom console failed");
    CHECK(touch_file("mock_sdcard/Roms/Game Boy Advance (GBA)/Metroid Fusion.gba"),
          "create direct rom failed");
    CHECK(make_dir_recursive("mock_sdcard/Roms/Collections"),
          "mkdir plain folder failed");
    CHECK(build_absolute_path(&env,
                              "mock_sdcard/Roms/Game Boy Advance (GBA)/Metroid Fusion.gba",
                              direct_rom_abs, sizeof(direct_rom_abs)),
          "direct rom abs path too long");
    CHECK(build_absolute_path(&env, "mock_sdcard/Roms/Collections",
                              plain_folder_abs, sizeof(plain_folder_abs)),
          "plain folder abs path too long");
    CHECK(build_resume_manifest_path(manifest_path, sizeof(manifest_path)),
          "manifest path too long");

    CHECK(set_hook_env("rom", direct_rom_abs), "set direct-rom hook env failed");
    CHECK(resume_sync_from_hook_env() == 0,
          "hook sync for direct rom failed");
    CHECK(!path_exists(manifest_path),
          "manifest should not exist after direct rom hook");

    CHECK(set_hook_env("rom", plain_folder_abs),
          "set plain-folder hook env failed");
    CHECK(resume_sync_from_hook_env() == 0,
          "hook sync for non-shortcut folder failed");
    CHECK(!path_exists(manifest_path),
          "manifest should not exist after non-shortcut folder hook");

    ok = true;

cleanup:
    clear_hook_env();
    teardown_test_env(&env);
    return ok;
}

static bool test_single_file_resume_sync_creates_alias(void)
{
    test_env env = {0};
    bool ok = false;
    char shortcut_name[SC_MAX_NAME];
    char shortcut_path[SC_MAX_PATH];
    char real_slot_path[SC_MAX_PATH];
    char alias_slot_path[SC_MAX_PATH];

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(create_rom_shortcut_fixture("Fusion", "GBA", SC_POS_ALPHA,
                                      "../Game Boy Advance (GBA)/Metroid Fusion.gba",
                                      shortcut_name, sizeof(shortcut_name),
                                      shortcut_path, sizeof(shortcut_path)),
          "create shortcut fixture failed");
    CHECK(build_real_slot_path("GBA", "Metroid Fusion.gba",
                               real_slot_path, sizeof(real_slot_path)),
          "real slot path too long");
    CHECK(build_alias_slot_path(shortcut_name, "GBA",
                                alias_slot_path, sizeof(alias_slot_path)),
          "alias slot path too long");
    CHECK(make_dir_recursive("mock_sdcard/.userdata/shared/.minui/GBA"),
          "mkdir gba slot dir failed");
    CHECK(write_file(real_slot_path, "3"), "write real slot failed");

    CHECK(resume_sync_for_shortcut_path(shortcut_path) == 0,
          "resume sync failed");
    CHECK(file_contents_equal(alias_slot_path, "3"),
          "alias slot content mismatch");
    CHECK(manifest_contains_entry(shortcut_path, alias_slot_path),
          "manifest missing single-file alias entry");

    ok = true;

cleanup:
    teardown_test_env(&env);
    return ok;
}

static bool test_cue_folder_resume_sync_creates_alias(void)
{
    test_env env = {0};
    bool ok = false;
    char shortcut_name[SC_MAX_NAME];
    char shortcut_path[SC_MAX_PATH];
    char real_slot_path[SC_MAX_PATH];
    char alias_slot_path[SC_MAX_PATH];

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(create_rom_shortcut_fixture("Formula 1 97", "PS", SC_POS_ALPHA,
                                      "../PlayStation (PS)/Formula 1 97/Formula 1 97.cue",
                                      shortcut_name, sizeof(shortcut_name),
                                      shortcut_path, sizeof(shortcut_path)),
          "create cue shortcut fixture failed");
    CHECK(build_real_slot_path("PS", "Formula 1 97.cue",
                               real_slot_path, sizeof(real_slot_path)),
          "real cue slot path too long");
    CHECK(build_alias_slot_path(shortcut_name, "PS",
                                alias_slot_path, sizeof(alias_slot_path)),
          "cue alias slot path too long");
    CHECK(make_dir_recursive("mock_sdcard/.userdata/shared/.minui/PS"),
          "mkdir ps slot dir failed");
    CHECK(write_file(real_slot_path, "5"), "write cue real slot failed");

    CHECK(resume_sync_for_shortcut_path(shortcut_path) == 0,
          "cue resume sync failed");
    CHECK(file_contents_equal(alias_slot_path, "5"),
          "cue alias slot content mismatch");
    CHECK(manifest_contains_entry(shortcut_path, alias_slot_path),
          "manifest missing cue alias entry");

    ok = true;

cleanup:
    teardown_test_env(&env);
    return ok;
}

static bool test_multidisc_resume_sync_uses_external_m3u_key(void)
{
    test_env env = {0};
    bool ok = false;
    char shortcut_name[SC_MAX_NAME];
    char shortcut_path[SC_MAX_PATH];
    char real_slot_path[SC_MAX_PATH];
    char alias_slot_path[SC_MAX_PATH];

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(create_rom_shortcut_fixture("Final Fantasy VII", "PS", SC_POS_BOTTOM,
                                      "../PlayStation (PS)/Final Fantasy VII/Final Fantasy VII.m3u",
                                      shortcut_name, sizeof(shortcut_name),
                                      shortcut_path, sizeof(shortcut_path)),
          "create multidisc shortcut fixture failed");
    CHECK(build_real_slot_path("PS", "Final Fantasy VII.m3u",
                               real_slot_path, sizeof(real_slot_path)),
          "real multidisc slot path too long");
    CHECK(build_alias_slot_path(shortcut_name, "PS",
                                alias_slot_path, sizeof(alias_slot_path)),
          "multidisc alias slot path too long");
    CHECK(make_dir_recursive("mock_sdcard/.userdata/shared/.minui/PS"),
          "mkdir ps slot dir failed");
    CHECK(write_file(real_slot_path, "2"), "write multidisc real slot failed");

    CHECK(resume_sync_for_shortcut_path(shortcut_path) == 0,
          "multidisc resume sync failed");
    CHECK(file_contents_equal(alias_slot_path, "2"),
          "multidisc alias slot content mismatch");
    CHECK(manifest_contains_entry(shortcut_path, alias_slot_path),
          "manifest missing multidisc alias entry");

    ok = true;

cleanup:
    teardown_test_env(&env);
    return ok;
}

static bool test_missing_real_slot_removes_alias(void)
{
    test_env env = {0};
    bool ok = false;
    char shortcut_name[SC_MAX_NAME];
    char shortcut_path[SC_MAX_PATH];
    char real_slot_path[SC_MAX_PATH];
    char alias_slot_path[SC_MAX_PATH];
    char manifest_path[SC_MAX_PATH];

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(create_rom_shortcut_fixture("Fusion", "GBA", SC_POS_ALPHA,
                                      "../Game Boy Advance (GBA)/Metroid Fusion.gba",
                                      shortcut_name, sizeof(shortcut_name),
                                      shortcut_path, sizeof(shortcut_path)),
          "create shortcut fixture failed");
    CHECK(build_real_slot_path("GBA", "Metroid Fusion.gba",
                               real_slot_path, sizeof(real_slot_path)),
          "real slot path too long");
    CHECK(build_alias_slot_path(shortcut_name, "GBA",
                                alias_slot_path, sizeof(alias_slot_path)),
          "alias slot path too long");
    CHECK(build_resume_manifest_path(manifest_path, sizeof(manifest_path)),
          "manifest path too long");
    CHECK(make_dir_recursive("mock_sdcard/.userdata/shared/.minui/GBA"),
          "mkdir gba slot dir failed");
    CHECK(write_file(real_slot_path, "4"), "write real slot failed");

    CHECK(resume_sync_for_shortcut_path(shortcut_path) == 0,
          "initial resume sync failed");
    CHECK(path_exists(alias_slot_path), "alias should exist after initial sync");

    CHECK(unlink(real_slot_path) == 0, "remove real slot failed");
    CHECK(resume_sync_for_shortcut_path(shortcut_path) == 0,
          "resume sync with missing real slot failed");
    CHECK(!path_exists(alias_slot_path),
          "alias should be removed when real slot is missing");
    CHECK(!path_exists(manifest_path),
          "manifest should be removed when last alias is gone");

    ok = true;

cleanup:
    teardown_test_env(&env);
    return ok;
}

static bool test_resume_alias_follows_rename_and_future_updates(void)
{
    test_env env = {0};
    bool ok = false;
    char old_shortcut_name[SC_MAX_NAME];
    char old_shortcut_path[SC_MAX_PATH];
    char new_shortcut_name[SC_MAX_NAME];
    char new_shortcut_path[SC_MAX_PATH];
    char real_slot_path[SC_MAX_PATH];
    char old_alias_slot_path[SC_MAX_PATH];
    char new_alias_slot_path[SC_MAX_PATH];
    shortcut_entry sc = {0};

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(create_rom_shortcut_fixture("Fusion", "GBA", SC_POS_ALPHA,
                                      "../Game Boy Advance (GBA)/Metroid Fusion.gba",
                                      old_shortcut_name, sizeof(old_shortcut_name),
                                      old_shortcut_path, sizeof(old_shortcut_path)),
          "create shortcut fixture failed");
    CHECK(build_folder_name(SC_POS_ALPHA, "Fusion Zero Mission", "GBA",
                            new_shortcut_name, sizeof(new_shortcut_name)),
          "build new shortcut name failed");
    CHECK(snprintf(new_shortcut_path, sizeof(new_shortcut_path),
                   "mock_sdcard/Roms/%s", new_shortcut_name) <
          (int)sizeof(new_shortcut_path),
          "new shortcut path too long");
    CHECK(build_real_slot_path("GBA", "Metroid Fusion.gba",
                               real_slot_path, sizeof(real_slot_path)),
          "real slot path too long");
    CHECK(build_alias_slot_path(old_shortcut_name, "GBA",
                                old_alias_slot_path, sizeof(old_alias_slot_path)),
          "old alias slot path too long");
    CHECK(build_alias_slot_path(new_shortcut_name, "GBA",
                                new_alias_slot_path, sizeof(new_alias_slot_path)),
          "new alias slot path too long");
    CHECK(make_dir_recursive("mock_sdcard/.userdata/shared/.minui/GBA"),
          "mkdir gba slot dir failed");
    CHECK(write_file(real_slot_path, "1"), "write initial real slot failed");
    CHECK(resume_sync_for_shortcut_path(old_shortcut_path) == 0,
          "initial resume sync failed");

    snprintf(sc.name, sizeof(sc.name), "%s", old_shortcut_name);
    snprintf(sc.tag, sizeof(sc.tag), "%s", "GBA");
    snprintf(sc.display, sizeof(sc.display), "%s", "Fusion");
    snprintf(sc.path, sizeof(sc.path), "%s", old_shortcut_path);

    CHECK(rename_shortcut(&sc, "Fusion Zero Mission") == 0,
          "rename shortcut failed");
    CHECK(!path_exists(old_alias_slot_path),
          "old alias should be removed after rename");
    CHECK(file_contents_equal(new_alias_slot_path, "1"),
          "new alias should reflect initial slot after rename");
    CHECK(manifest_contains_entry(new_shortcut_path, new_alias_slot_path),
          "manifest missing renamed alias entry");

    CHECK(write_file(real_slot_path, "2"), "write updated real slot failed");
    CHECK(resume_sync_for_shortcut_path(new_shortcut_path) == 0,
          "resume sync after rename failed");
    CHECK(file_contents_equal(new_alias_slot_path, "2"),
          "renamed alias did not track later slot updates");

    ok = true;

cleanup:
    teardown_test_env(&env);
    return ok;
}

static bool test_remove_shortcut_cleans_resume_aliases(void)
{
    test_env env = {0};
    bool ok = false;
    char shortcut_name[SC_MAX_NAME];
    char shortcut_path[SC_MAX_PATH];
    char real_slot_path[SC_MAX_PATH];
    char alias_slot_path[SC_MAX_PATH];
    char manifest_path[SC_MAX_PATH];

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(create_rom_shortcut_fixture("Fusion", "GBA", SC_POS_ALPHA,
                                      "../Game Boy Advance (GBA)/Metroid Fusion.gba",
                                      shortcut_name, sizeof(shortcut_name),
                                      shortcut_path, sizeof(shortcut_path)),
          "create shortcut fixture failed");
    CHECK(build_real_slot_path("GBA", "Metroid Fusion.gba",
                               real_slot_path, sizeof(real_slot_path)),
          "real slot path too long");
    CHECK(build_alias_slot_path(shortcut_name, "GBA",
                                alias_slot_path, sizeof(alias_slot_path)),
          "alias slot path too long");
    CHECK(build_resume_manifest_path(manifest_path, sizeof(manifest_path)),
          "manifest path too long");
    CHECK(make_dir_recursive("mock_sdcard/.userdata/shared/.minui/GBA"),
          "mkdir gba slot dir failed");
    CHECK(write_file(real_slot_path, "6"), "write real slot failed");
    CHECK(resume_sync_for_shortcut_path(shortcut_path) == 0,
          "resume sync failed");

    CHECK(remove_shortcut(shortcut_path) == 0, "remove_shortcut failed");
    CHECK(!path_exists(shortcut_path), "shortcut folder should be deleted");
    CHECK(!path_exists(alias_slot_path), "alias slot should be deleted");
    CHECK(!path_exists(manifest_path), "manifest should be removed");

    ok = true;

cleanup:
    teardown_test_env(&env);
    return ok;
}

static bool test_remove_shortcut_skips_tampered_manifest_alias_path(void)
{
    test_env env = {0};
    bool ok = false;
    char shortcut_name[SC_MAX_NAME];
    char shortcut_path[SC_MAX_PATH];
    char real_slot_path[SC_MAX_PATH];
    char alias_slot_path[SC_MAX_PATH];
    char manifest_path[SC_MAX_PATH];
    char manifest_content[SC_MAX_PATH * 2];

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(create_rom_shortcut_fixture("Fusion", "GBA", SC_POS_ALPHA,
                                      "../Game Boy Advance (GBA)/Metroid Fusion.gba",
                                      shortcut_name, sizeof(shortcut_name),
                                      shortcut_path, sizeof(shortcut_path)),
          "create shortcut fixture failed");
    CHECK(build_real_slot_path("GBA", "Metroid Fusion.gba",
                               real_slot_path, sizeof(real_slot_path)),
          "real slot path too long");
    CHECK(build_alias_slot_path(shortcut_name, "GBA",
                                alias_slot_path, sizeof(alias_slot_path)),
          "alias slot path too long");
    CHECK(build_resume_manifest_path(manifest_path, sizeof(manifest_path)),
          "manifest path too long");
    CHECK(make_dir_recursive("mock_sdcard/.userdata/shared/.minui/GBA"),
          "mkdir gba slot dir failed");
    CHECK(write_file(real_slot_path, "6"), "write real slot failed");
    CHECK(resume_sync_for_shortcut_path(shortcut_path) == 0,
          "resume sync failed");
    CHECK(make_dir_recursive("outside"), "mkdir outside failed");
    CHECK(write_file("outside/keep.txt", "keep"), "write outside marker failed");
    CHECK(snprintf(manifest_content, sizeof(manifest_content),
                   "%s\toutside/keep.txt\n", shortcut_path) <
          (int)sizeof(manifest_content),
          "manifest content too long");
    CHECK(write_file(manifest_path, manifest_content),
          "write tampered manifest failed");

    CHECK(remove_shortcut(shortcut_path) == 0, "remove_shortcut failed");
    CHECK(path_exists("outside/keep.txt"),
          "tampered manifest alias path should not be deleted");
    CHECK(!path_exists(shortcut_path), "shortcut folder should be deleted");
    CHECK(!path_exists(alias_slot_path), "expected alias slot should be deleted");
    CHECK(!path_exists(manifest_path), "manifest should be removed");

    ok = true;

cleanup:
    teardown_test_env(&env);
    return ok;
}

static bool test_external_delete_is_pruned_on_startup(void)
{
    test_env env = {0};
    bool ok = false;
    char shortcut_name[SC_MAX_NAME];
    char shortcut_path[SC_MAX_PATH];
    char real_slot_path[SC_MAX_PATH];
    char alias_slot_path[SC_MAX_PATH];
    char manifest_path[SC_MAX_PATH];

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(create_rom_shortcut_fixture("Fusion", "GBA", SC_POS_ALPHA,
                                      "../Game Boy Advance (GBA)/Metroid Fusion.gba",
                                      shortcut_name, sizeof(shortcut_name),
                                      shortcut_path, sizeof(shortcut_path)),
          "create shortcut fixture failed");
    CHECK(build_real_slot_path("GBA", "Metroid Fusion.gba",
                               real_slot_path, sizeof(real_slot_path)),
          "real slot path too long");
    CHECK(build_alias_slot_path(shortcut_name, "GBA",
                                alias_slot_path, sizeof(alias_slot_path)),
          "alias slot path too long");
    CHECK(build_resume_manifest_path(manifest_path, sizeof(manifest_path)),
          "manifest path too long");
    CHECK(make_dir_recursive("mock_sdcard/.userdata/shared/.minui/GBA"),
          "mkdir gba slot dir failed");
    CHECK(write_file(real_slot_path, "7"), "write real slot failed");
    CHECK(resume_sync_for_shortcut_path(shortcut_path) == 0,
          "resume sync failed");
    CHECK(remove_tree(shortcut_path), "external shortcut delete failed");

    CHECK(resume_sync_prune_aliases() == 0, "startup prune failed");
    CHECK(!path_exists(alias_slot_path), "alias slot should be pruned");
    CHECK(!path_exists(manifest_path), "manifest should be removed after prune");

    ok = true;

cleanup:
    teardown_test_env(&env);
    return ok;
}

static bool test_prune_skips_tampered_manifest_alias_path(void)
{
    test_env env = {0};
    bool ok = false;
    char shortcut_name[SC_MAX_NAME];
    char shortcut_path[SC_MAX_PATH];
    char real_slot_path[SC_MAX_PATH];
    char alias_slot_path[SC_MAX_PATH];
    char manifest_path[SC_MAX_PATH];
    char manifest_content[SC_MAX_PATH * 2];

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(create_rom_shortcut_fixture("Fusion", "GBA", SC_POS_ALPHA,
                                      "../Game Boy Advance (GBA)/Metroid Fusion.gba",
                                      shortcut_name, sizeof(shortcut_name),
                                      shortcut_path, sizeof(shortcut_path)),
          "create shortcut fixture failed");
    CHECK(build_real_slot_path("GBA", "Metroid Fusion.gba",
                               real_slot_path, sizeof(real_slot_path)),
          "real slot path too long");
    CHECK(build_alias_slot_path(shortcut_name, "GBA",
                                alias_slot_path, sizeof(alias_slot_path)),
          "alias slot path too long");
    CHECK(build_resume_manifest_path(manifest_path, sizeof(manifest_path)),
          "manifest path too long");
    CHECK(make_dir_recursive("mock_sdcard/.userdata/shared/.minui/GBA"),
          "mkdir gba slot dir failed");
    CHECK(write_file(real_slot_path, "7"), "write real slot failed");
    CHECK(resume_sync_for_shortcut_path(shortcut_path) == 0,
          "resume sync failed");
    CHECK(make_dir_recursive("outside"), "mkdir outside failed");
    CHECK(write_file("outside/keep.txt", "keep"), "write outside marker failed");
    CHECK(snprintf(manifest_content, sizeof(manifest_content),
                   "%s\toutside/keep.txt\n", shortcut_path) <
          (int)sizeof(manifest_content),
          "manifest content too long");
    CHECK(write_file(manifest_path, manifest_content),
          "write tampered manifest failed");
    CHECK(remove_tree(shortcut_path), "external shortcut delete failed");

    CHECK(resume_sync_prune_aliases() == 0, "startup prune failed");
    CHECK(path_exists("outside/keep.txt"),
          "tampered manifest alias path should not be pruned");
    CHECK(path_exists(alias_slot_path),
          "unreferenced expected alias slot should be left untouched");
    CHECK(!path_exists(manifest_path),
          "tampered manifest row should be removed after prune");

    ok = true;

cleanup:
    teardown_test_env(&env);
    return ok;
}

static bool test_external_delete_is_pruned_on_hook_run(void)
{
    test_env env = {0};
    bool ok = false;
    char shortcut_name[SC_MAX_NAME];
    char shortcut_path[SC_MAX_PATH];
    char real_slot_path[SC_MAX_PATH];
    char alias_slot_path[SC_MAX_PATH];
    char manifest_path[SC_MAX_PATH];
    char direct_rom_abs[SC_MAX_PATH];

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(create_rom_shortcut_fixture("Fusion", "GBA", SC_POS_ALPHA,
                                      "../Game Boy Advance (GBA)/Metroid Fusion.gba",
                                      shortcut_name, sizeof(shortcut_name),
                                      shortcut_path, sizeof(shortcut_path)),
          "create shortcut fixture failed");
    CHECK(make_dir_recursive("mock_sdcard/Roms/Game Boy Advance (GBA)"),
          "mkdir rom console failed");
    CHECK(touch_file("mock_sdcard/Roms/Game Boy Advance (GBA)/Zero Mission.gba"),
          "create direct rom failed");
    CHECK(build_absolute_path(&env,
                              "mock_sdcard/Roms/Game Boy Advance (GBA)/Zero Mission.gba",
                              direct_rom_abs, sizeof(direct_rom_abs)),
          "direct rom abs path too long");
    CHECK(build_real_slot_path("GBA", "Metroid Fusion.gba",
                               real_slot_path, sizeof(real_slot_path)),
          "real slot path too long");
    CHECK(build_alias_slot_path(shortcut_name, "GBA",
                                alias_slot_path, sizeof(alias_slot_path)),
          "alias slot path too long");
    CHECK(build_resume_manifest_path(manifest_path, sizeof(manifest_path)),
          "manifest path too long");
    CHECK(make_dir_recursive("mock_sdcard/.userdata/shared/.minui/GBA"),
          "mkdir gba slot dir failed");
    CHECK(write_file(real_slot_path, "8"), "write real slot failed");
    CHECK(resume_sync_for_shortcut_path(shortcut_path) == 0,
          "resume sync failed");
    CHECK(remove_tree(shortcut_path), "external shortcut delete failed");
    CHECK(set_hook_env("rom", direct_rom_abs), "set hook env failed");

    CHECK(resume_sync_from_hook_env() == 0, "hook-driven prune failed");
    CHECK(!path_exists(alias_slot_path), "alias slot should be pruned by hook");
    CHECK(!path_exists(manifest_path),
          "manifest should be removed by hook-driven prune");

    ok = true;

cleanup:
    clear_hook_env();
    teardown_test_env(&env);
    return ok;
}

static bool test_tool_shortcuts_never_create_resume_aliases(void)
{
    test_env env = {0};
    bool ok = false;
    char tool_shortcut_path[SC_MAX_PATH];
    char manifest_path[SC_MAX_PATH];

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(create_tool_shortcut_fixture("RetroArch", SC_POS_ALPHA,
                                       tool_shortcut_path, sizeof(tool_shortcut_path)),
          "create tool shortcut fixture failed");
    CHECK(build_resume_manifest_path(manifest_path, sizeof(manifest_path)),
          "manifest path too long");

    CHECK(resume_sync_for_shortcut_path(tool_shortcut_path) == 0,
          "tool shortcut resume sync should no-op");
    CHECK(!path_exists(manifest_path),
          "tool shortcuts should not create manifest entries");

    ok = true;

cleanup:
    teardown_test_env(&env);
    return ok;
}

static bool test_rename_shortcut_updates_layout_for_all_positions(void)
{
    typedef struct {
        sc_position pos;
        const char *label;
    } rename_case;

    static const rename_case cases[] = {
        { SC_POS_BOTTOM, "bottom" },
        { SC_POS_TOP, "top" },
        { SC_POS_ALPHA, "alpha" },
    };

    test_env env = {0};
    bool ok = false;

    CHECK(setup_test_env(&env), "setup failed");

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const char *old_display = "Old Name";
        const char *new_display = "New Name";
        const char *tag = "TAG";
        char old_folder_name[SC_MAX_NAME];
        char new_folder_name[SC_MAX_NAME];
        char old_folder_path[SC_MAX_PATH];
        char new_folder_path[SC_MAX_PATH];
        char old_m3u_path[SC_MAX_PATH * 2];
        char new_m3u_path[SC_MAX_PATH * 2];
        char new_marker_path[SC_MAX_PATH * 2];
        char old_thumb_path[SC_MAX_PATH * 2];
        char new_thumb_path[SC_MAX_PATH * 2];
        shortcut_entry sc = {0};

        CHECK(build_folder_name(cases[i].pos, old_display, tag,
                                old_folder_name, sizeof(old_folder_name)),
              "build old folder name failed for %s", cases[i].label);
        CHECK(build_folder_name(cases[i].pos, new_display, tag,
                                new_folder_name, sizeof(new_folder_name)),
              "build new folder name failed for %s", cases[i].label);
        CHECK(snprintf(old_folder_path, sizeof(old_folder_path),
                       "mock_sdcard/Roms/%s", old_folder_name) <
              (int)sizeof(old_folder_path),
              "old folder path too long for %s", cases[i].label);
        CHECK(snprintf(new_folder_path, sizeof(new_folder_path),
                       "mock_sdcard/Roms/%s", new_folder_name) <
              (int)sizeof(new_folder_path),
              "new folder path too long for %s", cases[i].label);
        CHECK(make_dir_recursive(old_folder_path),
              "mkdir shortcut fixture failed for %s", cases[i].label);
        CHECK(snprintf(old_m3u_path, sizeof(old_m3u_path), "%s/%s.m3u",
                       old_folder_path, old_folder_name) <
              (int)sizeof(old_m3u_path),
              "old m3u path too long for %s", cases[i].label);
        CHECK(snprintf(new_m3u_path, sizeof(new_m3u_path), "%s/%s.m3u",
                       new_folder_path, new_folder_name) <
              (int)sizeof(new_m3u_path),
              "new m3u path too long for %s", cases[i].label);
        CHECK(snprintf(new_marker_path, sizeof(new_marker_path), "%s/.shortcut",
                       new_folder_path) < (int)sizeof(new_marker_path),
              "marker path too long for %s", cases[i].label);
        CHECK(make_dir_recursive("mock_sdcard/Roms/.media"),
              "mkdir root media failed for %s", cases[i].label);
        CHECK(snprintf(old_thumb_path, sizeof(old_thumb_path),
                       "mock_sdcard/Roms/.media/%s.png", old_folder_name) <
              (int)sizeof(old_thumb_path),
              "old thumbnail path too long for %s", cases[i].label);
        CHECK(snprintf(new_thumb_path, sizeof(new_thumb_path),
                       "mock_sdcard/Roms/.media/%s.png", new_folder_name) <
              (int)sizeof(new_thumb_path),
              "new thumbnail path too long for %s", cases[i].label);
        CHECK(write_file(old_m3u_path, "../Console (TAG)/Old Name.zip"),
              "write m3u fixture failed for %s", cases[i].label);
        CHECK(write_file(old_thumb_path, "THUMB"),
              "write root thumbnail fixture failed for %s", cases[i].label);
        {
            char marker_path[SC_MAX_PATH * 2];
            CHECK(snprintf(marker_path, sizeof(marker_path), "%s/.shortcut",
                           old_folder_path) < (int)sizeof(marker_path),
                  "old marker path too long for %s", cases[i].label);
            CHECK(write_file(marker_path, old_display),
                  "write marker fixture failed for %s", cases[i].label);
        }

        snprintf(sc.name, sizeof(sc.name), "%s", old_folder_name);
        snprintf(sc.tag, sizeof(sc.tag), "%s", tag);
        snprintf(sc.display, sizeof(sc.display), "%s", old_display);
        snprintf(sc.path, sizeof(sc.path), "%s", old_folder_path);

        CHECK(rename_shortcut(&sc, new_display) == 0,
              "rename failed for %s", cases[i].label);
        CHECK(!path_exists(old_folder_path),
              "old folder still exists for %s", cases[i].label);
        CHECK(path_is_dir(new_folder_path),
              "new folder missing for %s", cases[i].label);
        CHECK(!path_exists(old_m3u_path),
              "old m3u still exists for %s", cases[i].label);
        CHECK(path_exists(new_m3u_path),
              "new m3u missing for %s", cases[i].label);
        CHECK(file_contents_equal(new_marker_path, new_display),
              "marker not updated for %s", cases[i].label);
        CHECK(!path_exists(old_thumb_path),
              "old root thumbnail still exists for %s", cases[i].label);
        CHECK(file_contents_equal(new_thumb_path, "THUMB"),
              "new root thumbnail missing for %s", cases[i].label);
    }

    ok = true;

cleanup:
    teardown_test_env(&env);
    return ok;
}

static bool test_renamed_shortcut_is_detected_by_rom_target(void)
{
    test_env env = {0};
    console_dir *consoles = NULL;
    rom_file *roms = NULL;
    shortcut_entry *shortcuts = NULL;
    int count = 0;
    bool ok = false;
    const console_dir *console = NULL;
    const rom_file *rom = NULL;
    const shortcut_entry *shortcut = NULL;
    shortcut_entry sc = {0};

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(make_dir_recursive("mock_sdcard/Roms/Game Boy Advance (GBA)"),
          "mkdir gba console failed");
    CHECK(touch_file("mock_sdcard/Roms/Game Boy Advance (GBA)/Metroid Fusion.gba"),
          "create fusion rom failed");
    CHECK(create_rom_shortcut_fixture("Metroid Fusion", "GBA", SC_POS_ALPHA,
                                      "../Game Boy Advance (GBA)/Metroid Fusion.gba",
                                      sc.name, sizeof(sc.name),
                                      sc.path, sizeof(sc.path)),
          "create shortcut fixture failed");
    snprintf(sc.tag, sizeof(sc.tag), "%s", "GBA");
    snprintf(sc.display, sizeof(sc.display), "%s", "Metroid Fusion");

    CHECK(rename_shortcut(&sc, "Fusion Zero Mission") == 0,
          "rename shortcut failed");

    CHECK(scan_console_dirs(false, &consoles, &count) == 0,
          "scan_console_dirs(false) failed");
    console = find_console_by_name(consoles, count, "Game Boy Advance (GBA)");
    CHECK(console != NULL, "missing gba console");
    CHECK(scan_roms(console->path, false, &roms, &count) == 0,
          "scan_roms(false) failed");
    rom = find_rom_by_name(roms, count, "Metroid Fusion.gba");
    CHECK(rom != NULL, "missing rom after rename");

    CHECK(scan_shortcuts(&shortcuts, &count) == 0,
          "scan_shortcuts failed");
    shortcut = find_shortcut_by_display(shortcuts, count, "Fusion Zero Mission");
    CHECK(shortcut != NULL, "missing renamed shortcut");
    CHECK(rom_matches_shortcut_target(rom, shortcut),
          "renamed shortcut should still match rom target");
    CHECK(!shortcut_exists(rom->display, console->tag),
          "display-based shortcut_exists should not match renamed shortcut");

    ok = true;

cleanup:
    free(shortcuts);
    free(roms);
    free(consoles);
    teardown_test_env(&env);
    return ok;
}

static bool test_rename_shortcut_with_slash_uses_storage_safe_name(void)
{
    test_env env = {0};
    bool ok = false;
    shortcut_entry *shortcuts = NULL;
    int count = 0;
    const shortcut_entry *shortcut = NULL;
    char marker_path[SC_MAX_PATH * 2];
    char m3u_path[SC_MAX_PATH * 2];
    shortcut_entry sc = {0};

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(create_rom_shortcut_fixture("Old Name", "TAG", SC_POS_ALPHA,
                                      "../Console (TAG)/Old Name.zip",
                                      sc.name, sizeof(sc.name),
                                      sc.path, sizeof(sc.path)),
          "create shortcut fixture failed");

    snprintf(sc.tag, sizeof(sc.tag), "%s", "TAG");
    snprintf(sc.display, sizeof(sc.display), "%s", "Old Name");

    CHECK(rename_shortcut(&sc, "New / Name") == 0,
          "rename shortcut with slash failed");
    CHECK(scan_shortcuts(&shortcuts, &count) == 0,
          "scan_shortcuts failed");
    shortcut = find_shortcut_by_display(shortcuts, count, "New / Name");
    CHECK(shortcut != NULL, "missing slash-renamed shortcut");
    CHECK(strstr(shortcut->name, "%2F") != NULL,
          "slash-renamed storage name should encode slash: %s",
          shortcut->name);
    CHECK(snprintf(marker_path, sizeof(marker_path), "%s/.shortcut",
                   shortcut->path) < (int)sizeof(marker_path),
          "slash marker path too long");
    CHECK(snprintf(m3u_path, sizeof(m3u_path), "%s/%s.m3u",
                   shortcut->path, shortcut->name) < (int)sizeof(m3u_path),
          "slash m3u path too long");
    CHECK(file_contents_equal(marker_path, "New / Name"),
          "slash rename did not preserve marker display");
    CHECK(path_exists(m3u_path), "slash-renamed m3u missing");

    ok = true;

cleanup:
    free(shortcuts);
    teardown_test_env(&env);
    return ok;
}

static bool test_hex_to_sc_color_parses_nextui_color_formats(void)
{
    bool ok = false;
    sc_color c;

    /* NextUI ≥ v6.12.0 format: 0xRRGGBBAA. Default black must stay black
     * (regression: it used to decode as pure blue). */
    c = hex_to_sc_color_for_tests("0x000000FF");
    CHECK(c.r == 0 && c.g == 0 && c.b == 0 && c.a == 255,
          "0x000000FF decoded as r=%u g=%u b=%u a=%u",
          c.r, c.g, c.b, c.a);

    c = hex_to_sc_color_for_tests("0x11223344");
    CHECK(c.r == 0x11 && c.g == 0x22 && c.b == 0x33 && c.a == 0x44,
          "0x11223344 decoded as r=%u g=%u b=%u a=%u",
          c.r, c.g, c.b, c.a);

    /* Legacy 0xRRGGBB format (bgcolor fallback on NextUI < v6.12.0). */
    c = hex_to_sc_color_for_tests("0x112233");
    CHECK(c.r == 0x11 && c.g == 0x22 && c.b == 0x33 && c.a == 255,
          "0x112233 decoded as r=%u g=%u b=%u a=%u",
          c.r, c.g, c.b, c.a);

    /* '#' prefix with alpha. */
    c = hex_to_sc_color_for_tests("#A1B2C3D4");
    CHECK(c.r == 0xA1 && c.g == 0xB2 && c.b == 0xC3 && c.a == 0xD4,
          "#A1B2C3D4 decoded as r=%u g=%u b=%u a=%u",
          c.r, c.g, c.b, c.a);

    /* Invalid input falls back to opaque black. */
    c = hex_to_sc_color_for_tests("");
    CHECK(c.r == 0 && c.g == 0 && c.b == 0 && c.a == 255,
          "empty string decoded as r=%u g=%u b=%u a=%u",
          c.r, c.g, c.b, c.a);
    c = hex_to_sc_color_for_tests(NULL);
    CHECK(c.r == 0 && c.g == 0 && c.b == 0 && c.a == 255,
          "NULL decoded as r=%u g=%u b=%u a=%u",
          c.r, c.g, c.b, c.a);
    c = hex_to_sc_color_for_tests("0x12345");
    CHECK(c.r == 0 && c.g == 0 && c.b == 0 && c.a == 255,
          "5-digit string decoded as r=%u g=%u b=%u a=%u",
          c.r, c.g, c.b, c.a);
    c = hex_to_sc_color_for_tests("0xZZZZZZZZ");
    CHECK(c.r == 0 && c.g == 0 && c.b == 0 && c.a == 255,
          "non-hex string decoded as r=%u g=%u b=%u a=%u",
          c.r, c.g, c.b, c.a);

    ok = true;

cleanup:
    return ok;
}

static bool test_compute_art_layout_matches_nextui_thumbnail_geometry(void)
{
    bool ok = false;
    int w, h, x, y, radius;

    /* Brick (1024x768, FIXED_SCALE=3), default artWidth=0.45, radius=20.
     * NextUI: max_w=460, 460x345 fits; x=1024-(460+45)=519; y=384-172=212. */
    compute_art_layout_for_tests(1024, 768, 460, 345, 0.45, 3, 20,
                                 &w, &h, &x, &y, &radius);
    CHECK(w == 460 && h == 345 && x == 519 && y == 212 && radius == 60,
          "brick layout: w=%d h=%d x=%d y=%d r=%d", w, h, x, y, radius);

    /* tg5040 non-Brick (1280x720, FIXED_SCALE=2): max_w=576, margin=30. */
    compute_art_layout_for_tests(1280, 720, 460, 345, 0.45, 2, 20,
                                 &w, &h, &x, &y, &radius);
    CHECK(w == 576 && h == 432 && x == 674 && y == 144 && radius == 40,
          "tg5040 layout: w=%d h=%d x=%d y=%d r=%d", w, h, x, y, radius);

    /* Tall art hits the max_h cap (0.6*768=460): 300x600 -> 230x460. */
    compute_art_layout_for_tests(1024, 768, 300, 600, 0.45, 3, 20,
                                 &w, &h, &x, &y, &radius);
    CHECK(w == 230 && h == 460 && x == 749 && y == 154 && radius == 60,
          "capped layout: w=%d h=%d x=%d y=%d r=%d", w, h, x, y, radius);

    /* Custom user settings: artWidth=40%%, radius=12 (Brick). */
    compute_art_layout_for_tests(1024, 768, 460, 345, 0.40, 3, 12,
                                 &w, &h, &x, &y, &radius);
    CHECK(w == 409 && h == 306 && x == 570 && y == 231 && radius == 36,
          "custom layout: w=%d h=%d x=%d y=%d r=%d", w, h, x, y, radius);

    ok = true;

cleanup:
    return ok;
}

int main(void)
{
    static const test_case tests[] = {
        { "sidecar-only console is hidden", test_sidecar_only_console_is_hidden },
        { "real rom ignores sidecars", test_real_rom_ignores_sidecars },
        { "nested rom console qualifies", test_nested_rom_console_qualifies },
        { "empty console does not qualify", test_empty_console_does_not_qualify },
        { "multidisc and cue folders survive", test_multidisc_and_cue_folders_survive },
        { "show_hidden reveals hidden roms not sidecars", test_show_hidden_reveals_hidden_roms_not_sidecars },
        { "collection console labels include tags", test_collection_console_labels_include_tags },
        { "map.txt resolves rom titles", test_map_txt_resolves_rom_titles },
        { "mapped shortcut creation preserves raw targets and artwork", test_mapped_shortcut_creation_preserves_raw_targets_and_artwork },
        { "regenerate disabled multidisc uses console artwork", test_regenerate_disabled_multidisc_uses_console_artwork },
        { "slash-mapped shortcut creation uses storage-safe names", test_slash_mapped_shortcut_creation_uses_storage_safe_names },
        { "ports .ports dir always hidden", test_ports_dotports_always_hidden },
        { "tool names preserve internal pak suffixes", test_tool_names_preserve_internal_pak_suffixes },
        { "tool shortcut m3u uses valid relative tool path", test_tool_shortcut_m3u_uses_valid_relative_tool_path },
        { "tool shortcut rejects pak outside sd root", test_tool_shortcut_rejects_pak_outside_sd_root },
        { "bridge launcher accepts file and dir targets", test_bridge_launcher_accepts_file_and_dir_targets },
        { "symlinked entries are skipped", test_symlinked_entries_are_skipped },
        { "ensure_dir_exists rejects file collisions", test_ensure_dir_exists_rejects_file_collisions },
        { "resume hook install is idempotent", test_resume_hook_install_is_idempotent },
        { "resume sync hook noops for pak launches", test_resume_sync_hook_noops_for_pak_launches },
        { "pak launch dedups recent without trailing newline", test_pak_launch_dedups_recent_without_trailing_newline },
        { "pak launch prunes game tracker tool shortcuts", test_pak_launch_prunes_game_tracker_tool_shortcuts },
        { "resume sync hook noops for direct roms and non shortcuts", test_resume_sync_hook_noops_for_direct_roms_and_non_shortcuts },
        { "single-file resume sync creates alias", test_single_file_resume_sync_creates_alias },
        { "cue-folder resume sync creates alias", test_cue_folder_resume_sync_creates_alias },
        { "multidisc resume sync uses external m3u key", test_multidisc_resume_sync_uses_external_m3u_key },
        { "missing real slot removes alias", test_missing_real_slot_removes_alias },
        { "resume alias follows rename and future updates", test_resume_alias_follows_rename_and_future_updates },
        { "remove shortcut cleans resume aliases", test_remove_shortcut_cleans_resume_aliases },
        { "remove shortcut skips tampered manifest alias path", test_remove_shortcut_skips_tampered_manifest_alias_path },
        { "external delete is pruned on startup", test_external_delete_is_pruned_on_startup },
        { "prune skips tampered manifest alias path", test_prune_skips_tampered_manifest_alias_path },
        { "external delete is pruned on hook run", test_external_delete_is_pruned_on_hook_run },
        { "tool shortcuts never create resume aliases", test_tool_shortcuts_never_create_resume_aliases },
        { "rename shortcut updates layout for all positions", test_rename_shortcut_updates_layout_for_all_positions },
        { "renamed shortcut is detected by rom target", test_renamed_shortcut_is_detected_by_rom_target },
        { "rename shortcut with slash uses storage-safe name", test_rename_shortcut_with_slash_uses_storage_safe_name },
        { "hex_to_sc_color parses NextUI color formats", test_hex_to_sc_color_parses_nextui_color_formats },
        { "art layout matches NextUI thumbnail geometry", test_compute_art_layout_matches_nextui_thumbnail_geometry },
    };
    int failures = 0;

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        bool passed = tests[i].fn();
        fprintf(stderr, "%s: %s\n", passed ? "PASS" : "FAIL", tests[i].name);
        if (!passed)
            failures++;
    }

    if (failures != 0) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }

    fprintf(stderr, "%zu test(s) passed\n",
            sizeof(tests) / sizeof(tests[0]));
    return 0;
}
