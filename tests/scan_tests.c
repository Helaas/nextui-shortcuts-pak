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

int main(void)
{
    static const test_case tests[] = {
        { "sidecar-only console is hidden", test_sidecar_only_console_is_hidden },
        { "real rom ignores sidecars", test_real_rom_ignores_sidecars },
        { "nested rom console qualifies", test_nested_rom_console_qualifies },
        { "empty console does not qualify", test_empty_console_does_not_qualify },
        { "multidisc and cue folders survive", test_multidisc_and_cue_folders_survive },
        { "show_hidden reveals hidden roms not sidecars", test_show_hidden_reveals_hidden_roms_not_sidecars },
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
