/*
 * shortcuts.h — Shared data structures, constants, and function declarations
 *               for the Shortcuts pak.
 */
#ifndef SHORTCUTS_H
#define SHORTCUTS_H

#include <stdbool.h>

/* ── Size limits ──────────────────────────────────────────────── */

#define SC_MAX_PATH    1024
#define SC_MAX_NAME     512
#define SC_MAX_TAG       64
#define SC_MAX_DISPLAY  512

/* ── Shortcut prefixes & markers ──────────────────────────────── */

/* U+FEFF Zero Width No-Break Space (UTF-8: EF BB BF).
 * Prepended to Bottom-position shortcut folder names so they sort after Z in
 * NextUI without any visible prefix. */
#define SHORTCUT_PREFIX         "\xEF\xBB\xBF"
#define SHORTCUT_PREFIX_LEN     3

/* Legacy prefix: U+2605 BLACK STAR + space (UTF-8: E2 98 85 20).
 * Retained for backward-compatible detection of old-style shortcuts. */
#define LEGACY_PREFIX           "\xE2\x98\x85 "
#define LEGACY_PREFIX_LEN       4

/* "0) " prefix for top-of-list shortcuts.
 * NextUI's trimSortingMeta strips "{digits}) " from display names. */
#define TOP_PREFIX              "0) "
#define TOP_PREFIX_LEN          3

/* Hidden marker file written inside every shortcut folder.
 * Its content is the clean display name. */
#define SHORTCUT_MARKER         ".shortcut"

/* Tag used for tool shortcuts (the bridge emulator tag). */
#define BRIDGE_EMU_TAG          "SHORTCUT"

/* ── Enumerations ─────────────────────────────────────────────── */

typedef enum {
    SC_POS_BOTTOM = 0,   /* U+FEFF prefix — sorts after Z */
    SC_POS_TOP    = 1,   /* "0) " prefix  — sorts before A */
    SC_POS_ALPHA  = 2,   /* No prefix     — alphabetical */
} sc_position;

typedef enum {
    ART_MODE_BLACK     = 0,  /* Art on black canvas; always writes bg.png */
    ART_MODE_WALLPAPER = 1,  /* Art on device wallpaper; always writes bg.png */
    ART_MODE_FALLBACK  = 2,  /* Art on wallpaper; skips when no art exists */
} art_mode;

/* ── Data structures ──────────────────────────────────────────── */

typedef struct {
    bool     copy_artwork;   /* Default: true */
    art_mode artwork_mode;   /* Default: ART_MODE_WALLPAPER */
    bool     show_hidden;    /* Default: false */
} app_settings;

typedef struct {
    char name[SC_MAX_NAME];
    char tag[SC_MAX_TAG];
    char path[SC_MAX_PATH];
    char display[SC_MAX_DISPLAY];
    bool is_disabled;
} console_dir;

typedef struct {
    char name[SC_MAX_NAME];
    char path[SC_MAX_PATH];
    char display[SC_MAX_DISPLAY];
    bool is_multi_disc;
    bool is_cue_folder;
    bool is_disabled;
} rom_file;

typedef struct {
    char name[SC_MAX_NAME];
    char path[SC_MAX_PATH];
    char display[SC_MAX_DISPLAY];
} tool_pak;

typedef struct {
    char name[SC_MAX_NAME];
    char tag[SC_MAX_TAG];
    char display[SC_MAX_DISPLAY];
    char path[SC_MAX_PATH];
    bool is_tool;
    char target_path[SC_MAX_PATH];
} shortcut_entry;

/* ── Main actions ─────────────────────────────────────────────── */

typedef enum {
    MAIN_ACTION_QUIT = 0,
    MAIN_ACTION_ADD_ROM,
    MAIN_ACTION_ADD_TOOL,
    MAIN_ACTION_MANAGE,
    MAIN_ACTION_MANAGE_MEDIA,
    MAIN_ACTION_SETTINGS,
} main_action;

typedef enum {
    DETAIL_ACTION_BACK = 0,
    DETAIL_ACTION_DELETED,
    DETAIL_ACTION_RENAMED,
} detail_action;

/* ── Globals (defined in main.c) ──────────────────────────────── */

extern bool g_is_brick;

/* ── device.c — String utilities ──────────────────────────────── */

bool starts_with(const char *str, const char *prefix);
bool ends_with(const char *str, const char *suffix);
void extract_tag(const char *name, char *out, int out_size);
void extract_display_name(const char *name, char *out, int out_size);
void strip_extension(const char *name, char *out, int out_size);
bool build_folder_name(sc_position pos, const char *display, const char *tag,
                       char *out, int out_size);
bool is_hidden(const char *name);
bool is_mac_dotfile(const char *name);
bool is_shortcut_folder(const char *folder_path);

/* ── device.c — Path resolution ───────────────────────────────── */

void get_roms_path(char *out, int out_size);
void get_tools_path(char *out, int out_size);
void get_emus_path(char *out, int out_size);
void get_settings_path(char *out, int out_size);
void get_global_bg_path(char *out, int out_size);
void get_screen_dimensions(int *w, int *h);

/* ── device.c — Settings ──────────────────────────────────────── */

app_settings load_settings(void);
int save_settings(const app_settings *s);
void artwork_bg_params(const app_settings *s, bool *use_global_bg, bool *force_black);

/* ── device.c — File I/O utilities ────────────────────────────── */

char *read_text_file(const char *path);
int write_text_file(const char *path, const char *content);
int ensure_dir_exists(const char *path);
int rmdir_recursive(const char *path);

/* ── device.c — Scanning ──────────────────────────────────────── */

int scan_console_dirs(bool show_hidden, console_dir **out, int *count);
int scan_roms(const char *console_path, bool show_hidden, rom_file **out, int *count);
int scan_tools(bool show_hidden, tool_pak **out, int *count);
int scan_shortcuts(shortcut_entry **out, int *count);

/* ── device.c — Shortcut CRUD ─────────────────────────────────── */

int create_rom_shortcut(const char *display_name, const char *tag,
                        const char *console_dir_name, const rom_file *rom,
                        sc_position pos, const app_settings *settings);
int create_tool_shortcut(const char *display_name, const char *pak_path,
                         sc_position pos, const app_settings *settings);
int remove_shortcut(const char *shortcut_path);
int rename_shortcut(const shortcut_entry *sc, const char *new_display);
bool shortcut_exists(const char *display_name, const char *tag);

/* ── device.c — Bridge emu ────────────────────────────────────── */

void ensure_bridge_emu(void);

/* ── artwork.c ────────────────────────────────────────────────── */

void generate_artwork_bg(const char *art_src_path, const char *dest_folder,
                         bool use_global_bg, bool force_black);
void shortcut_art_src_path(const shortcut_entry *sc, char *out, int out_size);
int regenerate_all_media(const app_settings *settings);
int remove_all_media(void);

/* ── ui.c ─────────────────────────────────────────────────────── */

main_action show_main_menu(void);
void add_rom_shortcut_flow(void);
void add_tool_shortcut_flow(void);
void manage_shortcuts_flow(void);
void manage_media_flow(void);
void show_settings_screen(void);

#endif /* SHORTCUTS_H */
