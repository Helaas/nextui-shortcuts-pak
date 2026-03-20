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

static bool file_contains_substring(const char *path, const char *needle)
{
    bool ok = false;
    char *data = read_text_file(path);

    if (!data) return false;
    ok = strstr(data, needle) != NULL;
    free(data);
    return ok;
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

    return make_dir_recursive("mock_sdcard/Roms");
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

static const shortcut_entry *find_shortcut_by_display(const shortcut_entry *shortcuts,
                                                      int count,
                                                      const char *display)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(shortcuts[i].display, display) == 0)
            return &shortcuts[i];
    }
    return NULL;
}

static bool create_rom_shortcut_fixture(const char *console_name,
                                        const char *rom_name,
                                        const char *shortcut_display,
                                        char *out_folder_name,
                                        size_t out_folder_name_size)
{
    char console_dir[SC_MAX_PATH];
    char rom_path[SC_MAX_PATH];
    char shortcut_dir[SC_MAX_PATH];
    char m3u_path[SC_MAX_PATH];
    char rel_target[SC_MAX_PATH];

    CHECK(build_folder_name(SC_POS_ALPHA, shortcut_display, "GB",
                            out_folder_name, (int)out_folder_name_size),
          "build_folder_name failed for %s", shortcut_display);
    CHECK(snprintf(console_dir, sizeof(console_dir), "mock_sdcard/Roms/%s",
                   console_name) < (int)sizeof(console_dir),
          "console dir path too long");
    CHECK(make_dir_recursive(console_dir), "create console dir failed");
    CHECK(snprintf(rom_path, sizeof(rom_path), "%s/%s", console_dir, rom_name) <
          (int)sizeof(rom_path), "rom path too long");
    CHECK(touch_file(rom_path), "create rom failed for %s", rom_name);

    CHECK(snprintf(shortcut_dir, sizeof(shortcut_dir), "mock_sdcard/Roms/%s",
                   out_folder_name) < (int)sizeof(shortcut_dir),
          "shortcut dir path too long");
    CHECK(make_dir_recursive(shortcut_dir), "create shortcut dir failed");
    CHECK(snprintf(m3u_path, sizeof(m3u_path), "%s/%s.m3u", shortcut_dir,
                   out_folder_name) < (int)sizeof(m3u_path),
          "shortcut m3u path too long");
    CHECK(snprintf(rel_target, sizeof(rel_target), "../%s/%s", console_name,
                   rom_name) < (int)sizeof(rel_target),
          "shortcut target path too long");
    CHECK(write_file(m3u_path, rel_target), "write shortcut m3u failed");

    CHECK(snprintf(m3u_path, sizeof(m3u_path), "%s/.shortcut", shortcut_dir) <
          (int)sizeof(m3u_path), "shortcut marker path too long");
    CHECK(write_file(m3u_path, shortcut_display), "write shortcut marker failed");
    return true;

cleanup:
    return false;
}

static bool create_tool_shortcut_fixture(const char *tool_name,
                                         const char *shortcut_display)
{
    char tool_dir[SC_MAX_PATH];
    char shortcut_dir[SC_MAX_PATH];
    char folder_name[SC_MAX_NAME];
    char path[SC_MAX_PATH];

    CHECK(snprintf(tool_dir, sizeof(tool_dir),
                   "mock_sdcard/Tools/tg5040/%s.pak", tool_name) <
          (int)sizeof(tool_dir), "tool dir path too long");
    CHECK(make_dir_recursive(tool_dir), "create tool dir failed");

    CHECK(build_folder_name(SC_POS_ALPHA, shortcut_display, BRIDGE_EMU_TAG,
                            folder_name, sizeof(folder_name)),
          "build tool shortcut folder name failed");
    CHECK(snprintf(shortcut_dir, sizeof(shortcut_dir), "mock_sdcard/Roms/%s",
                   folder_name) < (int)sizeof(shortcut_dir),
          "tool shortcut dir path too long");
    CHECK(make_dir_recursive(shortcut_dir), "create tool shortcut dir failed");

    CHECK(snprintf(path, sizeof(path), "%s/%s.m3u", shortcut_dir, folder_name) <
          (int)sizeof(path), "tool shortcut m3u path too long");
    CHECK(write_file(path, "target"), "write tool shortcut m3u failed");
    CHECK(snprintf(path, sizeof(path), "%s/target", shortcut_dir) <
          (int)sizeof(path), "tool shortcut target path too long");
    CHECK(write_file(path, tool_dir), "write tool shortcut target failed");
    CHECK(snprintf(path, sizeof(path), "%s/.shortcut", shortcut_dir) <
          (int)sizeof(path), "tool shortcut marker path too long");
    CHECK(write_file(path, shortcut_display), "write tool shortcut marker failed");
    return true;

cleanup:
    return false;
}

static bool test_resume_sync_aliases_rom_shortcuts_only(void)
{
    test_env env = {0};
    char shortcut_folder[SC_MAX_NAME];
    char real_slot[SC_MAX_PATH];
    char alias_slot[SC_MAX_PATH];
    bool ok = false;

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(create_rom_shortcut_fixture("Game Boy (GB)", "Tetris.gb",
                                      "Shortcut Tetris",
                                      shortcut_folder, sizeof(shortcut_folder)),
          "create rom shortcut fixture failed");
    CHECK(create_tool_shortcut_fixture("Retroarch", "Shortcut Tool"),
          "create tool shortcut fixture failed");
    CHECK(make_dir_recursive("mock_sdcard/.userdata/shared/.minui/GB"),
          "create minui dir failed");

    CHECK(snprintf(real_slot, sizeof(real_slot),
                   "mock_sdcard/.userdata/shared/.minui/GB/Tetris.gb.txt") <
          (int)sizeof(real_slot), "real slot path too long");
    CHECK(write_file(real_slot, "3"), "write real slot failed");

    CHECK(resume_sync_once() == 0, "resume_sync_once failed");

    CHECK(snprintf(alias_slot, sizeof(alias_slot),
                   "mock_sdcard/.userdata/shared/.minui/GB/%s.m3u.txt",
                   shortcut_folder) < (int)sizeof(alias_slot),
          "alias slot path too long");
    CHECK(file_contents_equal(alias_slot, "3"),
          "alias slot was not mirrored");

    CHECK(!path_exists("mock_sdcard/.userdata/shared/.minui/SHORTCUT/Shortcut Tool (SHORTCUT).m3u.txt"),
          "tool shortcut should not create alias slot");

    ok = true;

cleanup:
    teardown_test_env(&env);
    return ok;
}

static bool test_resume_sync_autostart_block_is_idempotent(void)
{
    test_env env = {0};
    char auto_path[SC_MAX_PATH];
    char *contents = NULL;
    bool ok = false;

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(make_dir_recursive("mock_sdcard/.userdata/tg5040"),
          "create device userdata dir failed");
    CHECK(write_file("mock_sdcard/.userdata/tg5040/auto.sh",
                     "#!/bin/sh\necho existing\n"),
          "seed auto.sh failed");

    ensure_resume_sync_autostart();
    ensure_resume_sync_autostart();

    CHECK(snprintf(auto_path, sizeof(auto_path),
                   "mock_sdcard/.userdata/tg5040/auto.sh") <
          (int)sizeof(auto_path), "auto path too long");
    contents = read_text_file(auto_path);
    CHECK(contents != NULL, "read auto.sh failed");
    CHECK(strstr(contents, "echo existing") != NULL,
          "auto.sh should preserve existing content");
    CHECK(strstr(contents, "# >>> shortcuts-resume-sync-managed >>>\n") != NULL,
          "auto.sh block missing");
    CHECK(strstr(strstr(contents, "# >>> shortcuts-resume-sync-managed >>>\n") + 1,
                 "# >>> shortcuts-resume-sync-managed >>>\n") == NULL,
          "auto.sh block duplicated");
    CHECK(strstr(contents, "--resume-sync-daemon") != NULL,
          "auto.sh block missing helper command");

    ok = true;

cleanup:
    free(contents);
    teardown_test_env(&env);
    return ok;
}

static bool test_rename_shortcut_keeps_resume_aliases_working(void)
{
    test_env env = {0};
    char original_folder[SC_MAX_NAME];
    char alias_old[SC_MAX_PATH];
    char alias_new[SC_MAX_PATH];
    char real_slot[SC_MAX_PATH];
    char manifest_path[SC_MAX_PATH];
    shortcut_entry *shortcuts = NULL;
    const shortcut_entry *shortcut = NULL;
    int count = 0;
    bool ok = false;

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(create_rom_shortcut_fixture("Game Boy (GB)", "Metroid.gb",
                                      "Metroid Shortcut",
                                      original_folder, sizeof(original_folder)),
          "create rom shortcut fixture failed");
    CHECK(make_dir_recursive("mock_sdcard/.userdata/shared/.minui/GB"),
          "create minui dir failed");
    CHECK(snprintf(real_slot, sizeof(real_slot),
                   "mock_sdcard/.userdata/shared/.minui/GB/Metroid.gb.txt") <
          (int)sizeof(real_slot), "real slot path too long");
    CHECK(snprintf(manifest_path, sizeof(manifest_path),
                   "mock_sdcard/.userdata/shared/Shortcuts/resume_aliases.tsv") <
          (int)sizeof(manifest_path), "manifest path too long");
    CHECK(write_file(real_slot, "5"), "write initial real slot failed");
    CHECK(resume_sync_once() == 0, "initial resume sync failed");

    CHECK(snprintf(alias_old, sizeof(alias_old),
                   "mock_sdcard/.userdata/shared/.minui/GB/%s.m3u.txt",
                   original_folder) < (int)sizeof(alias_old),
          "old alias path too long");
    CHECK(file_contents_equal(alias_old, "5"),
          "old alias missing before rename");

    CHECK(scan_shortcuts(&shortcuts, &count) == 0, "scan_shortcuts failed");
    shortcut = find_shortcut_by_display(shortcuts, count, "Metroid Shortcut");
    CHECK(shortcut != NULL, "could not find shortcut to rename");
    CHECK(rename_shortcut(shortcut, "Renamed Metroid") == 0,
          "rename_shortcut failed");
    free(shortcuts);
    shortcuts = NULL;

    CHECK(!path_exists(alias_old), "old alias should be removed after rename");

    CHECK(scan_shortcuts(&shortcuts, &count) == 0,
          "scan_shortcuts after rename failed");
    shortcut = find_shortcut_by_display(shortcuts, count, "Renamed Metroid");
    CHECK(shortcut != NULL, "renamed shortcut missing");
    CHECK(snprintf(alias_new, sizeof(alias_new),
                   "mock_sdcard/.userdata/shared/.minui/GB/%s.m3u.txt",
                   shortcut->name) < (int)sizeof(alias_new),
          "new alias path too long");
    CHECK(file_contents_equal(alias_new, "5"),
          "new alias missing immediately after rename");
    CHECK(write_file(real_slot, "7"), "update real slot after rename failed");
    CHECK(resume_sync_once() == 0, "resume sync after rename failed");
    CHECK(file_contents_equal(alias_new, "7"),
          "new alias should track later resume updates");
    CHECK(!file_contains_substring(manifest_path, alias_old),
          "manifest should not contain old alias after rename");
    CHECK(file_contains_substring(manifest_path, alias_new),
          "manifest should contain new alias after rename");

    ok = true;

cleanup:
    free(shortcuts);
    teardown_test_env(&env);
    return ok;
}

static bool test_remove_shortcut_cleans_resume_aliases(void)
{
    test_env env = {0};
    char original_folder[SC_MAX_NAME];
    char alias_path[SC_MAX_PATH];
    char real_slot[SC_MAX_PATH];
    char manifest_path[SC_MAX_PATH];
    shortcut_entry *shortcuts = NULL;
    const shortcut_entry *shortcut = NULL;
    int count = 0;
    bool ok = false;

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(create_rom_shortcut_fixture("Game Boy (GB)", "Metroid.gb",
                                      "Metroid Shortcut",
                                      original_folder, sizeof(original_folder)),
          "create rom shortcut fixture failed");
    CHECK(make_dir_recursive("mock_sdcard/.userdata/shared/.minui/GB"),
          "create minui dir failed");
    CHECK(snprintf(real_slot, sizeof(real_slot),
                   "mock_sdcard/.userdata/shared/.minui/GB/Metroid.gb.txt") <
          (int)sizeof(real_slot), "real slot path too long");
    CHECK(snprintf(alias_path, sizeof(alias_path),
                   "mock_sdcard/.userdata/shared/.minui/GB/%s.m3u.txt",
                   original_folder) < (int)sizeof(alias_path),
          "alias path too long");
    CHECK(snprintf(manifest_path, sizeof(manifest_path),
                   "mock_sdcard/.userdata/shared/Shortcuts/resume_aliases.tsv") <
          (int)sizeof(manifest_path), "manifest path too long");
    CHECK(write_file(real_slot, "5"), "write real slot failed");
    CHECK(resume_sync_once() == 0, "initial resume sync failed");
    CHECK(file_contents_equal(alias_path, "5"), "alias missing before delete");
    CHECK(file_contains_substring(manifest_path, alias_path),
          "manifest should contain alias before delete");

    CHECK(scan_shortcuts(&shortcuts, &count) == 0, "scan_shortcuts failed");
    shortcut = find_shortcut_by_display(shortcuts, count, "Metroid Shortcut");
    CHECK(shortcut != NULL, "could not find shortcut to remove");
    CHECK(remove_shortcut(shortcut->path) == 0, "remove_shortcut failed");

    CHECK(!path_exists(alias_path), "alias should be removed after delete");
    CHECK(!file_contains_substring(manifest_path, alias_path),
          "manifest should not contain alias after delete");

    ok = true;

cleanup:
    free(shortcuts);
    teardown_test_env(&env);
    return ok;
}

static bool test_external_shortcut_delete_cleans_resume_aliases(void)
{
    test_env env = {0};
    char original_folder[SC_MAX_NAME];
    char alias_path[SC_MAX_PATH];
    char real_slot[SC_MAX_PATH];
    char manifest_path[SC_MAX_PATH];
    shortcut_entry *shortcuts = NULL;
    const shortcut_entry *shortcut = NULL;
    int count = 0;
    bool ok = false;

    CHECK(setup_test_env(&env), "setup failed");
    CHECK(create_rom_shortcut_fixture("Game Boy (GB)", "Metroid.gb",
                                      "Metroid Shortcut",
                                      original_folder, sizeof(original_folder)),
          "create rom shortcut fixture failed");
    CHECK(make_dir_recursive("mock_sdcard/.userdata/shared/.minui/GB"),
          "create minui dir failed");
    CHECK(snprintf(real_slot, sizeof(real_slot),
                   "mock_sdcard/.userdata/shared/.minui/GB/Metroid.gb.txt") <
          (int)sizeof(real_slot), "real slot path too long");
    CHECK(snprintf(alias_path, sizeof(alias_path),
                   "mock_sdcard/.userdata/shared/.minui/GB/%s.m3u.txt",
                   original_folder) < (int)sizeof(alias_path),
          "alias path too long");
    CHECK(snprintf(manifest_path, sizeof(manifest_path),
                   "mock_sdcard/.userdata/shared/Shortcuts/resume_aliases.tsv") <
          (int)sizeof(manifest_path), "manifest path too long");
    CHECK(write_file(real_slot, "5"), "write real slot failed");
    CHECK(resume_sync_once() == 0, "initial resume sync failed");
    CHECK(file_contains_substring(manifest_path, alias_path),
          "manifest should contain alias before external delete");

    CHECK(scan_shortcuts(&shortcuts, &count) == 0, "scan_shortcuts failed");
    shortcut = find_shortcut_by_display(shortcuts, count, "Metroid Shortcut");
    CHECK(shortcut != NULL, "could not find shortcut to delete externally");
    CHECK(remove_tree(shortcut->path), "external delete of shortcut folder failed");
    CHECK(resume_sync_once() == 0, "resume sync after external delete failed");

    CHECK(!path_exists(alias_path),
          "alias should be removed after external shortcut delete");
    CHECK(!file_contains_substring(manifest_path, alias_path),
          "manifest should not contain alias after external delete");

    ok = true;

cleanup:
    free(shortcuts);
    teardown_test_env(&env);
    return ok;
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
        CHECK(write_file(old_m3u_path, "../Console (TAG)/Old Name.zip"),
              "write m3u fixture failed for %s", cases[i].label);
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
    }

    ok = true;

cleanup:
    teardown_test_env(&env);
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
        { "ports .ports dir always hidden", test_ports_dotports_always_hidden },
        { "tool names preserve internal pak suffixes", test_tool_names_preserve_internal_pak_suffixes },
        { "symlinked entries are skipped", test_symlinked_entries_are_skipped },
        { "ensure_dir_exists rejects file collisions", test_ensure_dir_exists_rejects_file_collisions },
        { "rename shortcut updates layout for all positions", test_rename_shortcut_updates_layout_for_all_positions },
        { "resume sync aliases rom shortcuts only", test_resume_sync_aliases_rom_shortcuts_only },
        { "resume sync auto.sh block is idempotent", test_resume_sync_autostart_block_is_idempotent },
        { "rename shortcut keeps resume aliases working", test_rename_shortcut_keeps_resume_aliases_working },
        { "remove shortcut cleans resume aliases", test_remove_shortcut_cleans_resume_aliases },
        { "external shortcut delete cleans resume aliases", test_external_shortcut_delete_cleans_resume_aliases },
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
