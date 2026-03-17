/*
 * ui.c — All Apostrophe UI flows: main menu, pickers, shortcut creation,
 *         management, media, and settings.
 */
#include "apostrophe.h"
#include "apostrophe_widgets.h"
#include "shortcuts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Utility screens ──────────────────────────────────────────── */

static void show_error(const char *message)
{
    ap_footer_item footer[] = {
        { .button = AP_BTN_B, .label = "Back" },
    };
    ap_message_opts opts = {
        .message = message,
        .footer = footer,
        .footer_count = 1,
    };
    ap_confirm_result result;
    ap_confirmation(&opts, &result);
}

static void show_info(const char *message)
{
    ap_footer_item footer[] = {
        { .button = AP_BTN_A, .label = "OK", .is_confirm = true },
    };
    ap_message_opts opts = {
        .message = message,
        .footer = footer,
        .footer_count = 1,
    };
    ap_confirm_result result;
    ap_confirmation(&opts, &result);
}

static bool show_confirm(const char *message, const char *confirm_label)
{
    ap_footer_item footer[] = {
        { .button = AP_BTN_B, .label = "Cancel" },
        { .button = AP_BTN_A, .label = confirm_label, .is_confirm = true },
    };
    ap_message_opts opts = {
        .message = message,
        .footer = footer,
        .footer_count = 2,
    };
    ap_confirm_result result = {0};
    int rc = ap_confirmation(&opts, &result);
    return rc == AP_OK && result.confirmed;
}

/* ── Main menu ────────────────────────────────────────────────── */

main_action show_main_menu(void)
{
    ap_list_item items[] = {
        AP_LIST_ITEM("Add ROM Shortcut",    NULL),
        AP_LIST_ITEM("Add Tool Shortcut",   NULL),
        AP_LIST_ITEM("Manage Shortcuts",    NULL),
        AP_LIST_ITEM("Manage Artwork",      NULL),
        AP_LIST_ITEM("Settings",            NULL),
    };
    ap_footer_item footer[] = {
        { .button = AP_BTN_B, .label = "Quit" },
        { .button = AP_BTN_A, .label = "Select", .is_confirm = true },
    };

    ap_list_opts opts = ap_list_default_opts("Shortcuts", items, 5);
    opts.footer = footer;
    opts.footer_count = 2;

    ap_list_result result = {0};
    int rc = ap_list(&opts, &result);
    if (rc == AP_CANCELLED || rc != AP_OK)
        return MAIN_ACTION_QUIT;

    switch (result.selected_index) {
    case 0: ap_log("ui: main menu -> add rom shortcut");    return MAIN_ACTION_ADD_ROM;
    case 1: ap_log("ui: main menu -> add tool shortcut");   return MAIN_ACTION_ADD_TOOL;
    case 2: ap_log("ui: main menu -> manage shortcuts");    return MAIN_ACTION_MANAGE;
    case 3: ap_log("ui: main menu -> manage artwork");      return MAIN_ACTION_MANAGE_MEDIA;
    case 4: ap_log("ui: main menu -> settings");            return MAIN_ACTION_SETTINGS;
    default: return MAIN_ACTION_QUIT;
    }
}

/* ── Position picker ──────────────────────────────────────────── */

static bool pick_position(sc_position *out)
{
    ap_list_item items[] = {
        AP_LIST_ITEM("Alphabetical",           NULL),
        AP_LIST_ITEM("Top         (before A)", NULL),
        AP_LIST_ITEM("Bottom  (after Z)",      NULL),
    };
    ap_footer_item footer[] = {
        { .button = AP_BTN_B, .label = "Back" },
        { .button = AP_BTN_A, .label = "Select", .is_confirm = true },
    };

    ap_list_opts opts = ap_list_default_opts("Shortcut Position", items, 3);
    opts.footer = footer;
    opts.footer_count = 2;

    ap_list_result result = {0};
    int rc = ap_list(&opts, &result);
    if (rc != AP_OK) return false;

    ap_log("ui: position picked: %d", result.selected_index);
    switch (result.selected_index) {
    case 1:  *out = SC_POS_TOP;    return true;
    case 2:  *out = SC_POS_BOTTOM; return true;
    default: *out = SC_POS_ALPHA;  return true;
    }
}

/* ── Console picker ───────────────────────────────────────────── */

static bool pick_console(console_dir *out)
{
    app_settings settings = load_settings();
    console_dir *consoles = NULL;
    int count = 0;

    if (scan_console_dirs(settings.show_hidden, &consoles, &count) != 0 ||
        count == 0) {
        show_error(count == 0 ? "No ROM folders found."
                              : "Could not read ROM folders.");
        free(consoles);
        return false;
    }

    /* Build list items. */
    ap_list_item *items = calloc(count, sizeof(ap_list_item));
    char (*labels)[SC_MAX_DISPLAY + 16] = calloc(count, sizeof(*labels));
    for (int i = 0; i < count; i++) {
        if (consoles[i].is_disabled)
            snprintf(labels[i], sizeof(labels[i]), "%s  [disabled]",
                     consoles[i].display);
        else
            snprintf(labels[i], sizeof(labels[i]), "%s", consoles[i].display);
        items[i].label = labels[i];
    }

    ap_footer_item footer[] = {
        { .button = AP_BTN_B, .label = "Back" },
        { .button = AP_BTN_A, .label = "Select", .is_confirm = true },
    };

    ap_list_opts opts = ap_list_default_opts("Select Console", items, count);
    opts.footer = footer;
    opts.footer_count = 2;

    ap_list_result result = {0};
    int rc = ap_list(&opts, &result);

    bool ok = false;
    if (rc == AP_OK && result.selected_index >= 0 &&
        result.selected_index < count) {
        *out = consoles[result.selected_index];
        ap_log("ui: selected console index=%d name=%s",
               result.selected_index, out->display);
        ok = true;
    }

    free(items);
    free(labels);
    free(consoles);
    return ok;
}

/* ── ROM picker ───────────────────────────────────────────────── */

static bool pick_rom(const console_dir *console, rom_file *out)
{
    app_settings settings = load_settings();
    rom_file *roms = NULL;
    int count = 0;

    if (scan_roms(console->path, settings.show_hidden, &roms, &count) != 0 ||
        count == 0) {
        char msg[SC_MAX_DISPLAY + 32];
        snprintf(msg, sizeof(msg), "No ROMs found in %s.", console->display);
        show_error(count == 0 ? msg : "Could not read ROMs.");
        free(roms);
        return false;
    }

    ap_list_item *items = calloc(count, sizeof(ap_list_item));
    char (*labels)[SC_MAX_DISPLAY + 64] = calloc(count, sizeof(*labels));
    for (int i = 0; i < count; i++) {
        char text[SC_MAX_DISPLAY + 64];
        text[0] = '\0';

        /* Prepend subfolder path if ROM is nested. */
        const char *rom_dir = roms[i].path;
        size_t console_len = strlen(console->path);
        /* Check if ROM's parent dir differs from the console dir. */
        char rom_parent[SC_MAX_PATH];
        snprintf(rom_parent, sizeof(rom_parent), "%s", rom_dir);
        char *last_slash = strrchr(rom_parent, '/');
        if (last_slash) *last_slash = '\0';

        if (strlen(rom_parent) > console_len &&
            strncmp(rom_parent, console->path, console_len) == 0) {
            /* ROM is in a subfolder — show relative subfolder path. */
            const char *sub = rom_parent + console_len + 1;
            snprintf(text, sizeof(text), "%s / %s", sub, roms[i].display);
        } else {
            snprintf(text, sizeof(text), "%s", roms[i].display);
        }

        if (roms[i].is_multi_disc)
            strncat(text, "  [Multi]", sizeof(text) - strlen(text) - 1);
        if (roms[i].is_cue_folder)
            strncat(text, "  [CUE]", sizeof(text) - strlen(text) - 1);
        if (roms[i].is_disabled)
            strncat(text, "  [disabled]", sizeof(text) - strlen(text) - 1);

        snprintf(labels[i], sizeof(labels[i]), "%s", text);
        items[i].label = labels[i];
    }

    ap_footer_item footer[] = {
        { .button = AP_BTN_B, .label = "Back" },
        { .button = AP_BTN_A, .label = "Select", .is_confirm = true },
    };

    ap_list_opts opts = ap_list_default_opts(console->display, items, count);
    opts.footer = footer;
    opts.footer_count = 2;

    ap_list_result result = {0};
    int rc = ap_list(&opts, &result);

    bool ok = false;
    if (rc == AP_OK && result.selected_index >= 0 &&
        result.selected_index < count) {
        *out = roms[result.selected_index];
        ap_log("ui: selected rom index=%d name=%s",
               result.selected_index, out->name);
        ok = true;
    }

    free(items);
    free(labels);
    free(roms);
    return ok;
}

/* ── Tool picker ──────────────────────────────────────────────── */

static bool pick_tool(tool_pak *out)
{
    app_settings settings = load_settings();
    tool_pak *tools = NULL;
    int count = 0;

    if (scan_tools(settings.show_hidden, &tools, &count) != 0 ||
        count == 0) {
        show_error(count == 0 ? "No tools found."
                              : "Could not read Tools folder.");
        free(tools);
        return false;
    }

    ap_list_item *items = calloc(count, sizeof(ap_list_item));
    char (*labels)[SC_MAX_DISPLAY] = calloc(count, sizeof(*labels));
    for (int i = 0; i < count; i++) {
        snprintf(labels[i], sizeof(labels[i]), "%s", tools[i].display);
        items[i].label = labels[i];
    }

    ap_footer_item footer[] = {
        { .button = AP_BTN_B, .label = "Back" },
        { .button = AP_BTN_A, .label = "Select", .is_confirm = true },
    };

    ap_list_opts opts = ap_list_default_opts("Select Tool", items, count);
    opts.footer = footer;
    opts.footer_count = 2;

    ap_list_result result = {0};
    int rc = ap_list(&opts, &result);

    bool ok = false;
    if (rc == AP_OK && result.selected_index >= 0 &&
        result.selected_index < count) {
        *out = tools[result.selected_index];
        ap_log("ui: selected tool index=%d name=%s",
               result.selected_index, out->name);
        ok = true;
    }

    free(items);
    free(labels);
    free(tools);
    return ok;
}

/* ── Worker structs for ap_process_message ────────────────────── */

typedef struct {
    char display_name[SC_MAX_DISPLAY];
    char tag[SC_MAX_TAG];
    char console_dir_name[SC_MAX_NAME];
    rom_file rom;
    sc_position pos;
    app_settings settings;
} create_rom_args;

static int create_rom_worker(void *userdata)
{
    create_rom_args *args = (create_rom_args *)userdata;
    return create_rom_shortcut(args->display_name, args->tag,
                               args->console_dir_name, &args->rom,
                               args->pos, &args->settings);
}

typedef struct {
    char display_name[SC_MAX_DISPLAY];
    char pak_path[SC_MAX_PATH];
    sc_position pos;
    app_settings settings;
} create_tool_args;

static int create_tool_worker(void *userdata)
{
    create_tool_args *args = (create_tool_args *)userdata;
    return create_tool_shortcut(args->display_name, args->pak_path,
                                args->pos, &args->settings);
}

typedef struct {
    char path[SC_MAX_PATH];
} remove_args;

static int remove_worker(void *userdata)
{
    remove_args *args = (remove_args *)userdata;
    return remove_shortcut(args->path);
}

typedef struct {
    app_settings settings;
} regen_args;

static int regen_worker(void *userdata)
{
    regen_args *args = (regen_args *)userdata;
    return regenerate_all_media(&args->settings);
}

static int remove_media_worker(void *userdata)
{
    (void)userdata;
    return remove_all_media();
}

/* ── Add ROM Shortcut flow ────────────────────────────────────── */

void add_rom_shortcut_flow(void)
{
    console_dir console;
    if (!pick_console(&console)) return;

    rom_file rom;
    if (!pick_rom(&console, &rom)) return;

    const char *display_name = rom.display;
    ap_log("ui: add rom shortcut: console=%s rom=%s multi=%d",
           console.display, rom.name, rom.is_multi_disc);

    if (shortcut_exists(display_name, console.tag)) {
        char msg[SC_MAX_DISPLAY + 64];
        snprintf(msg, sizeof(msg),
                 "A shortcut for \"%s\" already exists.", display_name);
        show_error(msg);
        return;
    }

    sc_position pos;
    if (!pick_position(&pos)) return;

    char folder_name[SC_MAX_NAME];
    build_folder_name(pos, display_name, console.tag,
                      folder_name, sizeof(folder_name));

    /* Confirmation message. */
    char rom_desc[SC_MAX_NAME + 32];
    snprintf(rom_desc, sizeof(rom_desc), "%s", rom.name);
    if (rom.is_multi_disc)
        snprintf(rom_desc, sizeof(rom_desc), "%s  [Multi-disc]", rom.name);
    else if (rom.is_cue_folder)
        snprintf(rom_desc, sizeof(rom_desc), "%s  [CUE folder]", rom.name);

    char msg[2048];
    snprintf(msg, sizeof(msg),
             "Create shortcut?\n\n%s\n\nConsole: %s\nROM: %s",
             folder_name, console.display, rom_desc);

    if (!show_confirm(msg, "Create")) return;

    /* Create the shortcut. */
    create_rom_args args;
    memset(&args, 0, sizeof(args));
    snprintf(args.display_name, sizeof(args.display_name), "%s", display_name);
    snprintf(args.tag, sizeof(args.tag), "%s", console.tag);
    snprintf(args.console_dir_name, sizeof(args.console_dir_name), "%s",
             console.name);
    args.rom = rom;
    args.pos = pos;
    args.settings = load_settings();

    ap_process_opts proc = { .message = "Creating shortcut..." };
    ap_process_message(&proc, create_rom_worker, &args);

    char done_msg[1024];
    snprintf(done_msg, sizeof(done_msg),
             "Shortcut created!\n\n%s\n\nwill appear on your main menu.",
             folder_name);
    show_info(done_msg);
}

/* ── Add Tool Shortcut flow ───────────────────────────────────── */

void add_tool_shortcut_flow(void)
{
    tool_pak tool;
    if (!pick_tool(&tool)) return;

    const char *display_name = tool.display;
    ap_log("ui: add tool shortcut: tool=%s", tool.name);

    if (shortcut_exists(display_name, BRIDGE_EMU_TAG)) {
        char msg[SC_MAX_DISPLAY + 64];
        snprintf(msg, sizeof(msg),
                 "A shortcut for \"%s\" already exists.", display_name);
        show_error(msg);
        return;
    }

    sc_position pos;
    if (!pick_position(&pos)) return;

    char folder_name[SC_MAX_NAME];
    build_folder_name(pos, display_name, BRIDGE_EMU_TAG,
                      folder_name, sizeof(folder_name));

    char msg[2048];
    snprintf(msg, sizeof(msg),
             "Create shortcut?\n\n%s\n\nTool: %s",
             folder_name, tool.name);

    if (!show_confirm(msg, "Create")) return;

    create_tool_args args;
    memset(&args, 0, sizeof(args));
    snprintf(args.display_name, sizeof(args.display_name), "%s", display_name);
    snprintf(args.pak_path, sizeof(args.pak_path), "%s", tool.path);
    args.pos = pos;
    args.settings = load_settings();

    ap_process_opts proc = { .message = "Creating shortcut..." };
    ap_process_message(&proc, create_tool_worker, &args);

    char done_msg[1024];
    snprintf(done_msg, sizeof(done_msg),
             "Shortcut created!\n\n%s\n\nwill appear on your main menu.",
             folder_name);
    show_info(done_msg);
}

/* ── Manage Shortcuts flow ────────────────────────────────────── */

static detail_action show_shortcut_detail(const shortcut_entry *sc)
{
    const char *kind = sc->is_tool ? "Tool" : "ROM";

    ap_detail_info_pair pairs[4];
    int pair_count = 0;
    pairs[pair_count++] = (ap_detail_info_pair){ .key = "Name",  .value = sc->display };
    pairs[pair_count++] = (ap_detail_info_pair){ .key = "Type",  .value = kind };
    pairs[pair_count++] = (ap_detail_info_pair){ .key = "Tag",   .value = sc->tag };
    if (sc->target_path[0] != '\0')
        pairs[pair_count++] = (ap_detail_info_pair){ .key = "Target", .value = sc->target_path };

    ap_detail_section sections[] = {
        {
            .type = AP_SECTION_INFO,
            .title = "Shortcut Info",
            .info_pairs = pairs,
            .info_count = pair_count,
        },
    };

    ap_footer_item footer[] = {
        { .button = AP_BTN_B, .label = "Back" },
        { .button = AP_BTN_A, .label = "Delete", .is_confirm = true },
    };

    ap_detail_opts opts = {
        .title = sc->display,
        .sections = sections,
        .section_count = 1,
        .footer = footer,
        .footer_count = 2,
        .center_title = true,
        .body_font = ap_get_font(AP_FONT_MEDIUM),
    };

    ap_detail_result result = {0};
    int rc = ap_detail_screen(&opts, &result);

    if (rc == AP_CANCELLED || result.action == AP_DETAIL_BACK)
        return DETAIL_ACTION_BACK;

    /* User pressed A — confirm deletion. */
    char msg[SC_MAX_DISPLAY + 128];
    snprintf(msg, sizeof(msg),
             "Delete shortcut?\n\n%s\n\nThis will remove the shortcut\nfrom the main menu.",
             sc->display);

    if (!show_confirm(msg, "Delete"))
        return DETAIL_ACTION_BACK;

    /* Delete the shortcut. */
    remove_args rargs;
    snprintf(rargs.path, sizeof(rargs.path), "%s", sc->path);

    ap_process_opts proc = { .message = "Removing shortcut..." };
    ap_process_message(&proc, remove_worker, &rargs);

    show_info("Shortcut removed.");
    return DETAIL_ACTION_DELETED;
}

void manage_shortcuts_flow(void)
{
    for (;;) {
        shortcut_entry *shortcuts = NULL;
        int count = 0;
        if (scan_shortcuts(&shortcuts, &count) != 0 || count == 0) {
            show_error(count == 0 ? "No shortcuts found.\n\nCreate one first!"
                                  : "Could not read shortcuts.");
            free(shortcuts);
            return;
        }

        ap_list_item *items = calloc(count, sizeof(ap_list_item));
        char (*labels)[SC_MAX_DISPLAY + 16] = calloc(count, sizeof(*labels));
        for (int i = 0; i < count; i++) {
            const char *kind = shortcuts[i].is_tool ? "Tool" : "ROM";
            snprintf(labels[i], sizeof(labels[i]), "%s  [%s]",
                     shortcuts[i].display, kind);
            items[i].label = labels[i];
        }

        ap_footer_item footer[] = {
            { .button = AP_BTN_B, .label = "Back" },
            { .button = AP_BTN_A, .label = "Details", .is_confirm = true },
        };

        ap_list_opts opts = ap_list_default_opts("Manage Shortcuts",
                                                  items, count);
        opts.footer = footer;
        opts.footer_count = 2;

        ap_list_result result = {0};
        int rc = ap_list(&opts, &result);

        if (rc != AP_OK || result.selected_index < 0 ||
            result.selected_index >= count) {
            free(items);
            free(labels);
            free(shortcuts);
            return;
        }

        int idx = result.selected_index;
        ap_log("ui: manage shortcuts -> selected index=%d name=%s",
               idx, shortcuts[idx].display);

        detail_action action = show_shortcut_detail(&shortcuts[idx]);

        free(items);
        free(labels);
        free(shortcuts);

        /* Refresh list after detail view (whether deleted or went back). */
        if (action == DETAIL_ACTION_DELETED || action == DETAIL_ACTION_BACK)
            continue;
    }
}

/* ── Manage Artwork flow ──────────────────────────────────────── */

static void regenerate_all_media_flow(void)
{
    if (!show_confirm(
            "Regenerate artwork for all shortcuts?\n\n"
            "This will (re)create bg.png for every\n"
            "shortcut using the current Artwork mode.",
            "Regenerate"))
        return;

    regen_args args;
    args.settings = load_settings();
    ap_process_opts proc = { .message = "Regenerating artwork..." };
    ap_process_message(&proc, regen_worker, &args);

    show_info("Artwork regenerated for all shortcuts.");
}

static void remove_all_media_flow(void)
{
    if (!show_confirm(
            "Remove all artwork?\n\n"
            "This will delete bg.png from every\n"
            "shortcut's .media folder.",
            "Remove"))
        return;

    ap_process_opts proc = { .message = "Removing artwork..." };
    ap_process_message(&proc, remove_media_worker, NULL);

    show_info("Artwork removed from all shortcuts.");
}

void manage_media_flow(void)
{
    ap_list_item items[] = {
        AP_LIST_ITEM("Regenerate artwork", NULL),
        AP_LIST_ITEM("Remove artwork",     NULL),
    };
    ap_footer_item footer[] = {
        { .button = AP_BTN_B, .label = "Back" },
        { .button = AP_BTN_A, .label = "Select", .is_confirm = true },
    };

    ap_list_opts opts = ap_list_default_opts("Manage Artwork", items, 2);
    opts.footer = footer;
    opts.footer_count = 2;

    ap_list_result result = {0};
    int rc = ap_list(&opts, &result);
    if (rc != AP_OK) return;

    switch (result.selected_index) {
    case 0: regenerate_all_media_flow(); break;
    case 1: remove_all_media_flow();     break;
    }
}

/* ── Settings screen ──────────────────────────────────────────── */

void show_settings_screen(void)
{
    app_settings settings = load_settings();

    /* Option: Copy artwork */
    ap_option copy_opts[] = {
        { .label = "Off", .value = "0" },
        { .label = "On",  .value = "1" },
    };
    /* Option: Artwork mode */
    ap_option mode_opts[] = {
        { .label = "Art on Black background",    .value = "0" },
        { .label = "Art on Main menu Wallpaper", .value = "1" },
        { .label = "Fallback to wallpaper",      .value = "2" },
    };
    /* Option: Show hidden */
    ap_option hidden_opts[] = {
        { .label = "Off", .value = "0" },
        { .label = "On",  .value = "1" },
    };

    ap_options_item items[] = {
        {
            .label = "Copy artwork when available",
            .options = copy_opts,
            .option_count = 2,
            .selected_option = settings.copy_artwork ? 1 : 0,
        },
        {
            .label = "Artwork mode",
            .options = mode_opts,
            .option_count = 3,
            .selected_option = (int)settings.artwork_mode,
        },
        {
            .label = "Show hidden/disabled/empty ROMs",
            .options = hidden_opts,
            .option_count = 2,
            .selected_option = settings.show_hidden ? 1 : 0,
        },
    };

    ap_footer_item footer[] = {
        { .button = AP_BTN_B, .label = "Back" },
        { .button = AP_BTN_LEFT, .label = "Change", .button_text = "←/→" },
        { .button = AP_BTN_A, .label = "Save", .is_confirm = true },
    };

    ap_options_list_opts opts = {
        .title = "Settings",
        .items = items,
        .item_count = 3,
        .footer = footer,
        .footer_count = 3,
        .confirm_button = AP_BTN_A,
    };

    ap_options_list_result result = {0};
    int rc = ap_options_list(&opts, &result);
    if (rc != AP_OK) return; /* B pressed — discard */

    /* Extract values. */
    settings.copy_artwork = (result.items[0].selected_option == 1);
    settings.artwork_mode = (art_mode)result.items[1].selected_option;
    settings.show_hidden  = (result.items[2].selected_option == 1);

    ap_log("ui: settings saving: copy_artwork=%d artwork_mode=%d show_hidden=%d",
           settings.copy_artwork, settings.artwork_mode, settings.show_hidden);
    if (save_settings(&settings) != 0) {
        ap_log("ui: settings save failed");
        show_error("Could not save settings.");
    }
}
