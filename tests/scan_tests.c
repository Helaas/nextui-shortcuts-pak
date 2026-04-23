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
        { "slash-mapped shortcut creation uses storage-safe names", test_slash_mapped_shortcut_creation_uses_storage_safe_names },
        { "ports .ports dir always hidden", test_ports_dotports_always_hidden },
        { "tool names preserve internal pak suffixes", test_tool_names_preserve_internal_pak_suffixes },
        { "symlinked entries are skipped", test_symlinked_entries_are_skipped },
        { "ensure_dir_exists rejects file collisions", test_ensure_dir_exists_rejects_file_collisions },
        { "rename shortcut updates layout for all positions", test_rename_shortcut_updates_layout_for_all_positions },
        { "renamed shortcut is detected by rom target", test_renamed_shortcut_is_detected_by_rom_target },
        { "rename shortcut with slash uses storage-safe name", test_rename_shortcut_with_slash_uses_storage_safe_name },
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
