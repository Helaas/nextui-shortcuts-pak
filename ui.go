package main

import (
	"fmt"
	"log"

	gaba "github.com/BrandonKowalski/gabagool/v2/pkg/gabagool"
	"github.com/BrandonKowalski/gabagool/v2/pkg/gabagool/constants"
)

// ── Main menu ────────────────────────────────────────────────

type mainAction int

const (
	mainActionQuit mainAction = iota
	mainActionAddROM
	mainActionAddTool
	mainActionManage
)

func showMainMenu() mainAction {
	items := []gaba.MenuItem{
		{Text: "Add ROM Shortcut"},
		{Text: "Add Tool Shortcut"},
		{Text: "Manage Shortcuts"},
	}

	opts := gaba.DefaultListOptions("Shortcuts", items)
	opts.FooterHelpItems = []gaba.FooterHelpItem{
		{ButtonName: "B", HelpText: "Quit"},
		{ButtonName: "A", HelpText: "Select"},
	}

	result, err := gaba.List(opts)
	if isErrCancelled(err) {
		return mainActionQuit
	}
	if err != nil {
		logError("main menu", err)
		return mainActionQuit
	}

	if len(result.Selected) == 0 {
		return mainActionQuit
	}

	switch result.Selected[0] {
	case 0:
		log.Printf("ui: main menu -> add rom shortcut")
		return mainActionAddROM
	case 1:
		log.Printf("ui: main menu -> add tool shortcut")
		return mainActionAddTool
	case 2:
		log.Printf("ui: main menu -> manage shortcuts")
		return mainActionManage
	default:
		return mainActionQuit
	}
}

// ── Add ROM Shortcut flow ────────────────────────────────────

func addROMShortcutFlow() {
	// Step 1: Pick a console
	console, ok := pickConsole()
	if !ok {
		return
	}

	// Step 2: Pick a ROM from that console
	rom, ok := pickROM(console)
	if !ok {
		return
	}

	// Step 3: Choose display name (default: ROM display name)
	displayName := rom.Display
	log.Printf("ui: add rom shortcut: console=%s rom=%s", console.Display, rom.Name)

	// Check if shortcut already exists
	if shortcutExists(displayName, console.Tag) {
		gaba.ConfirmationMessage(
			fmt.Sprintf("A shortcut for \"%s\" already exists.", displayName),
			[]gaba.FooterHelpItem{
				{ButtonName: "B", HelpText: "Back"},
			},
			gaba.MessageOptions{},
		)
		return
	}

	// Step 4: Confirm creation
	msg := fmt.Sprintf("Create shortcut?\n\n%s %s\n\nConsole: %s\nROM: %s",
		shortcutPrefix, displayName, console.Display, rom.Name)

	result, err := gaba.ConfirmationMessage(msg,
		[]gaba.FooterHelpItem{
			{ButtonName: "B", HelpText: "Cancel"},
			{ButtonName: "A", HelpText: "Create", IsConfirmButton: true},
		},
		gaba.MessageOptions{
			ConfirmButton: constants.VirtualButtonA,
		},
	)
	if isErrCancelled(err) || result == nil || !result.Confirmed {
		return
	}

	// Step 5: Create the shortcut
	gaba.ProcessMessage("Creating shortcut...",
		gaba.ProcessMessageOptions{ShowThemeBackground: true},
		func() (any, error) {
			return nil, createROMShortcut(displayName, console.Tag, console.Name, rom.Name)
		},
	)

	gaba.ConfirmationMessage(
		fmt.Sprintf("Shortcut created!\n\n%s %s will appear on\nyour main menu.", shortcutPrefix, displayName),
		[]gaba.FooterHelpItem{
			{ButtonName: "A", HelpText: "OK", IsConfirmButton: true},
		},
		gaba.MessageOptions{
			ConfirmButton: constants.VirtualButtonA,
		},
	)
}

func pickConsole() (ConsoleDir, bool) {
	consoles, err := scanConsoleDirs()
	if err != nil {
		logError("scanning consoles", err)
		showError("Could not read ROM folders.")
		return ConsoleDir{}, false
	}
	if len(consoles) == 0 {
		showError("No ROM folders found.")
		return ConsoleDir{}, false
	}

	items := make([]gaba.MenuItem, len(consoles))
	for i, c := range consoles {
		items[i] = gaba.MenuItem{Text: c.Display}
	}

	opts := gaba.DefaultListOptions("Select Console", items)
	opts.FooterHelpItems = []gaba.FooterHelpItem{
		{ButtonName: "B", HelpText: "Back"},
		{ButtonName: "A", HelpText: "Select"},
	}

	result, err := gaba.List(opts)
	if isErrCancelled(err) {
		return ConsoleDir{}, false
	}
	if err != nil || len(result.Selected) == 0 {
		return ConsoleDir{}, false
	}

	log.Printf("ui: selected console index=%d name=%s", result.Selected[0], consoles[result.Selected[0]].Display)
	return consoles[result.Selected[0]], true
}

func pickROM(console ConsoleDir) (ROMFile, bool) {
	roms, err := scanROMs(console.Path)
	if err != nil {
		logError("scanning ROMs", err)
		showError("Could not read ROMs.")
		return ROMFile{}, false
	}
	if len(roms) == 0 {
		showError(fmt.Sprintf("No ROMs found in %s.", console.Display))
		return ROMFile{}, false
	}

	items := make([]gaba.MenuItem, len(roms))
	for i, r := range roms {
		items[i] = gaba.MenuItem{Text: r.Display}
	}

	opts := gaba.DefaultListOptions(console.Display, items)
	opts.FooterHelpItems = []gaba.FooterHelpItem{
		{ButtonName: "B", HelpText: "Back"},
		{ButtonName: "A", HelpText: "Select"},
	}

	result, err := gaba.List(opts)
	if isErrCancelled(err) {
		return ROMFile{}, false
	}
	if err != nil || len(result.Selected) == 0 {
		return ROMFile{}, false
	}

	log.Printf("ui: selected rom index=%d name=%s", result.Selected[0], roms[result.Selected[0]].Name)
	return roms[result.Selected[0]], true
}

// ── Add Tool Shortcut flow ───────────────────────────────────

func addToolShortcutFlow() {
	tool, ok := pickTool()
	if !ok {
		return
	}

	displayName := tool.Display
	log.Printf("ui: add tool shortcut: tool=%s", tool.Name)

	// Check if shortcut already exists
	if shortcutExists(displayName, bridgeEmuTag) {
		gaba.ConfirmationMessage(
			fmt.Sprintf("A shortcut for \"%s\" already exists.", displayName),
			[]gaba.FooterHelpItem{
				{ButtonName: "B", HelpText: "Back"},
			},
			gaba.MessageOptions{},
		)
		return
	}

	// Confirm creation
	msg := fmt.Sprintf("Create shortcut?\n\n%s %s\n\nTool: %s",
		shortcutPrefix, displayName, tool.Name)

	result, err := gaba.ConfirmationMessage(msg,
		[]gaba.FooterHelpItem{
			{ButtonName: "B", HelpText: "Cancel"},
			{ButtonName: "A", HelpText: "Create", IsConfirmButton: true},
		},
		gaba.MessageOptions{
			ConfirmButton: constants.VirtualButtonA,
		},
	)
	if isErrCancelled(err) || result == nil || !result.Confirmed {
		return
	}

	// Create shortcut
	gaba.ProcessMessage("Creating shortcut...",
		gaba.ProcessMessageOptions{ShowThemeBackground: true},
		func() (any, error) {
			return nil, createToolShortcut(displayName, tool.Path)
		},
	)

	gaba.ConfirmationMessage(
		fmt.Sprintf("Shortcut created!\n\n%s %s will appear on\nyour main menu.", shortcutPrefix, displayName),
		[]gaba.FooterHelpItem{
			{ButtonName: "A", HelpText: "OK", IsConfirmButton: true},
		},
		gaba.MessageOptions{
			ConfirmButton: constants.VirtualButtonA,
		},
	)
}

func pickTool() (ToolPak, bool) {
	tools, err := scanTools()
	if err != nil {
		logError("scanning tools", err)
		showError("Could not read Tools folder.")
		return ToolPak{}, false
	}
	if len(tools) == 0 {
		showError("No tools found.")
		return ToolPak{}, false
	}

	items := make([]gaba.MenuItem, len(tools))
	for i, t := range tools {
		items[i] = gaba.MenuItem{Text: t.Display}
	}

	opts := gaba.DefaultListOptions("Select Tool", items)
	opts.FooterHelpItems = []gaba.FooterHelpItem{
		{ButtonName: "B", HelpText: "Back"},
		{ButtonName: "A", HelpText: "Select"},
	}

	result, err := gaba.List(opts)
	if isErrCancelled(err) {
		return ToolPak{}, false
	}
	if err != nil || len(result.Selected) == 0 {
		return ToolPak{}, false
	}

	log.Printf("ui: selected tool index=%d name=%s", result.Selected[0], tools[result.Selected[0]].Name)
	return tools[result.Selected[0]], true
}

// ── Manage existing shortcuts ────────────────────────────────

func manageShortcutsFlow() {
	for {
		shortcuts, err := scanShortcuts()
		if err != nil {
			logError("scanning shortcuts", err)
			showError("Could not read shortcuts.")
			return
		}
		if len(shortcuts) == 0 {
			showError("No shortcuts found.\n\nCreate one first!")
			return
		}

		items := make([]gaba.MenuItem, len(shortcuts))
		for i, sc := range shortcuts {
			kind := "ROM"
			if sc.IsTool {
				kind = "Tool"
			}
			items[i] = gaba.MenuItem{Text: fmt.Sprintf("%s  [%s]", sc.Display, kind)}
		}

		opts := gaba.DefaultListOptions("Manage Shortcuts", items)
		opts.FooterHelpItems = []gaba.FooterHelpItem{
			{ButtonName: "B", HelpText: "Back"},
			{ButtonName: "A", HelpText: "Details"},
		}

		result, err := gaba.List(opts)
		if isErrCancelled(err) {
			return
		}
		if err != nil || len(result.Selected) == 0 {
			return
		}

		idx := result.Selected[0]
		log.Printf("ui: manage shortcuts -> selected index=%d name=%s", idx, shortcuts[idx].Display)
		action := showShortcutDetail(shortcuts[idx])
		if action == detailActionDeleted || action == detailActionBack {
			// Refresh the list (deleted or went back from detail)
			continue
		}
	}
}

type detailAction int

const (
	detailActionBack    detailAction = iota
	detailActionDeleted
)

func showShortcutDetail(sc Shortcut) detailAction {
	kind := "ROM"
	if sc.IsTool {
		kind = "Tool"
	}

	metadata := []gaba.MetadataItem{
		{Label: "Name", Value: sc.Display},
		{Label: "Type", Value: kind},
		{Label: "Tag", Value: sc.Tag},
	}

	if sc.TargetPath != "" {
		metadata = append(metadata, gaba.MetadataItem{
			Label: "Target", Value: sc.TargetPath,
		})
	}

	sections := []gaba.Section{
		gaba.NewInfoSection("Shortcut Info", metadata),
	}

	detailOpts := gaba.DefaultInfoScreenOptions()
	detailOpts.Sections = sections
	detailOpts.ShowThemeBackground = true
	detailOpts.ShowScrollbar = false
	detailOpts.ConfirmButton = constants.VirtualButtonA

	footer := []gaba.FooterHelpItem{
		{ButtonName: "B", HelpText: "Back"},
		{ButtonName: "A", HelpText: "Delete", IsConfirmButton: true},
	}

	_, err := gaba.DetailScreen(sc.Display, detailOpts, footer)
	if isErrCancelled(err) {
		return detailActionBack
	}

	// User pressed A — confirm deletion
	return confirmDelete(sc)
}

func confirmDelete(sc Shortcut) detailAction {
	msg := fmt.Sprintf("Delete shortcut?\n\n%s\n\nThis will remove the shortcut\nfrom the main menu.", sc.Display)

	result, err := gaba.ConfirmationMessage(msg,
		[]gaba.FooterHelpItem{
			{ButtonName: "B", HelpText: "Cancel"},
			{ButtonName: "A", HelpText: "Delete", IsConfirmButton: true},
		},
		gaba.MessageOptions{
			ConfirmButton: constants.VirtualButtonA,
		},
	)
	if isErrCancelled(err) || result == nil || !result.Confirmed {
		return detailActionBack
	}

	// Delete the shortcut
	gaba.ProcessMessage("Removing shortcut...",
		gaba.ProcessMessageOptions{ShowThemeBackground: true},
		func() (any, error) {
			return nil, removeShortcut(sc.Path)
		},
	)

	gaba.ConfirmationMessage(
		"Shortcut removed.",
		[]gaba.FooterHelpItem{
			{ButtonName: "A", HelpText: "OK", IsConfirmButton: true},
		},
		gaba.MessageOptions{
			ConfirmButton: constants.VirtualButtonA,
		},
	)

	return detailActionDeleted
}

// ── Utility screens ──────────────────────────────────────────

func showError(message string) {
	gaba.ConfirmationMessage(message,
		[]gaba.FooterHelpItem{
			{ButtonName: "B", HelpText: "Back"},
		},
		gaba.MessageOptions{},
	)
}
