package main

import (
	"fmt"
	"log"
	"path/filepath"

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
	mainActionManageMedia
	mainActionSettings
)

func showMainMenu() mainAction {
	items := []gaba.MenuItem{
		{Text: "Add ROM Shortcut"},
		{Text: "Add Tool Shortcut"},
		{Text: "Manage Shortcuts"},
		{Text: "Manage Artwork"},
		{Text: "Settings"},
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
	case 3:
		log.Printf("ui: main menu -> manage artwork")
		return mainActionManageMedia
	case 4:
		log.Printf("ui: main menu -> settings")
		return mainActionSettings
	default:
		return mainActionQuit
	}
}

// ── Position picker ──────────────────────────────────────────

// pickPosition presents a list for choosing where the shortcut will sort in the menu.
func pickPosition() (ShortcutPosition, bool) {
	items := []gaba.MenuItem{
		{Text: "Alphabetical"},
		{Text: "Top         (before A)"},
		{Text: "Bottom  (after Z)"},
	}
	opts := gaba.DefaultListOptions("Shortcut Position", items)
	opts.FooterHelpItems = []gaba.FooterHelpItem{
		{ButtonName: "B", HelpText: "Back"},
		{ButtonName: "A", HelpText: "Select"},
	}
	result, err := gaba.List(opts)
	if isErrCancelled(err) {
		return ShortcutPositionBottom, false
	}
	if err != nil || len(result.Selected) == 0 {
		return ShortcutPositionBottom, false
	}
	log.Printf("ui: position picked: %d", result.Selected[0])
	switch result.Selected[0] {
	case 1:
		return ShortcutPositionTop, true
	case 2:
		return ShortcutPositionBottom, true
	default:
		return ShortcutPositionAlpha, true
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

	displayName := rom.Display
	log.Printf("ui: add rom shortcut: console=%s rom=%s multiDisc=%v", console.Display, rom.Name, rom.IsMultiDisc)

	// Step 3: Check if shortcut already exists
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

	// Step 4: Pick position
	pos, ok := pickPosition()
	if !ok {
		return
	}

	folderName := buildFolderName(pos, displayName, console.Tag)

	// Step 5: Confirm creation
	romDesc := rom.Name
	switch {
	case rom.IsMultiDisc:
		romDesc = rom.Name + "  [Multi-disc]"
	case rom.IsCueFolder:
		romDesc = rom.Name + "  [CUE folder]"
	}
	msg := fmt.Sprintf("Create shortcut?\n\n%s\n\nConsole: %s\nROM: %s",
		folderName, console.Display, romDesc)

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

	// Step 6: Create the shortcut
	settings := loadSettings()
	gaba.ProcessMessage("Creating shortcut...",
		gaba.ProcessMessageOptions{ShowThemeBackground: true},
		func() (any, error) {
			return nil, createROMShortcut(displayName, console.Tag, console.Name, rom, pos, settings)
		},
	)

	gaba.ConfirmationMessage(
		fmt.Sprintf("Shortcut created!\n\n%s\n\nwill appear on your main menu.", folderName),
		[]gaba.FooterHelpItem{
			{ButtonName: "A", HelpText: "OK", IsConfirmButton: true},
		},
		gaba.MessageOptions{
			ConfirmButton: constants.VirtualButtonA,
		},
	)
}

func pickConsole() (ConsoleDir, bool) {
	settings := loadSettings()
	consoles, err := scanConsoleDirs(settings.ShowHidden)
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
		text := c.Display
		if c.IsDisabled {
			text += "  [disabled]"
		}
		items[i] = gaba.MenuItem{Text: text}
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
	settings := loadSettings()
	roms, err := scanROMs(console.Path, settings.ShowHidden)
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
		text := r.Display
		if romDir := filepath.Dir(r.Path); romDir != console.Path {
			subDir, _ := filepath.Rel(console.Path, romDir)
			text = subDir + " / " + text
		}
		switch {
		case r.IsMultiDisc:
			text += "  [Multi]"
		case r.IsCueFolder:
			text += "  [CUE]"
		}
		if r.IsDisabled {
			text += "  [disabled]"
		}
		items[i] = gaba.MenuItem{Text: text}
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

	// Pick position
	pos, ok := pickPosition()
	if !ok {
		return
	}

	folderName := buildFolderName(pos, displayName, bridgeEmuTag)

	// Confirm creation
	msg := fmt.Sprintf("Create shortcut?\n\n%s\n\nTool: %s",
		folderName, tool.Name)

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
	settings := loadSettings()
	gaba.ProcessMessage("Creating shortcut...",
		gaba.ProcessMessageOptions{ShowThemeBackground: true},
		func() (any, error) {
			return nil, createToolShortcut(displayName, tool.Path, pos, settings)
		},
	)

	gaba.ConfirmationMessage(
		fmt.Sprintf("Shortcut created!\n\n%s\n\nwill appear on your main menu.", folderName),
		[]gaba.FooterHelpItem{
			{ButtonName: "A", HelpText: "OK", IsConfirmButton: true},
		},
		gaba.MessageOptions{
			ConfirmButton: constants.VirtualButtonA,
		},
	)
}

func pickTool() (ToolPak, bool) {
	settings := loadSettings()
	tools, err := scanTools(settings.ShowHidden)
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
	detailActionBack detailAction = iota
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

// ── Settings screen ──────────────────────────────────────────

// showSettingsScreen presents the global settings screen.
// Users cycle Left/Right to change values and press A to save, or B to discard.
func showSettingsScreen() {
	settings := loadSettings()

	initialArtwork := 0
	if settings.CopyArtwork {
		initialArtwork = 1
	}

	initialShowHidden := 0
	if settings.ShowHidden {
		initialShowHidden = 1
	}

	items := []gaba.ItemWithOptions{
		{
			Item: gaba.MenuItem{Text: "Copy artwork when available"},
			Options: []gaba.Option{
				{DisplayName: "Off", Value: false},
				{DisplayName: "On", Value: true},
			},
			SelectedOption: initialArtwork,
		},
		{
			Item: gaba.MenuItem{Text: "Artwork mode"},
			Options: []gaba.Option{
				{DisplayName: "Art on Black background", Value: ArtworkModeBlack},
				{DisplayName: "Art on Main menu Wallpaper", Value: ArtworkModeWallpaper},
				{DisplayName: "Fallback to wallpaper", Value: ArtworkModeFallback},
			},
			SelectedOption: settings.ArtworkMode,
		},
		{
			Item: gaba.MenuItem{Text: "Show hidden/disabled/empty ROMs"},
			Options: []gaba.Option{
				{DisplayName: "Off", Value: false},
				{DisplayName: "On", Value: true},
			},
			SelectedOption: initialShowHidden,
		},
	}

	listOpts := gaba.OptionListSettings{
		ConfirmButton: constants.VirtualButtonA,
		FooterHelpItems: []gaba.FooterHelpItem{
			{ButtonName: "B", HelpText: "Back"},
			{ButtonName: "←/→", HelpText: "Change"},
			{ButtonName: "A", HelpText: "Save"},
		},
	}

	result, err := gaba.OptionsList("Settings", listOpts, items)
	if isErrCancelled(err) {
		return // B pressed — discard changes
	}
	if err != nil {
		logError("settings screen", err)
		return
	}

	if result != nil {
		settings.CopyArtwork, _ = result.Items[0].Options[result.Items[0].SelectedOption].Value.(bool)
		settings.ArtworkMode, _ = result.Items[1].Options[result.Items[1].SelectedOption].Value.(int)
		settings.ShowHidden, _ = result.Items[2].Options[result.Items[2].SelectedOption].Value.(bool)
		log.Printf("ui: settings saving: copyArtwork=%v artworkMode=%d showHidden=%v",
			settings.CopyArtwork, settings.ArtworkMode, settings.ShowHidden)
		logError("saving settings", saveSettings(settings))
	}
}

// ── Media management flow ────────────────────────────────────

func manageMediaFlow() {
	items := []gaba.MenuItem{
		{Text: "Regenerate artwork"},
		{Text: "Remove artwork"},
	}

	opts := gaba.DefaultListOptions("Manage Artwork", items)
	opts.FooterHelpItems = []gaba.FooterHelpItem{
		{ButtonName: "B", HelpText: "Back"},
		{ButtonName: "A", HelpText: "Select"},
	}

	result, err := gaba.List(opts)
	if isErrCancelled(err) || err != nil || len(result.Selected) == 0 {
		return
	}

	switch result.Selected[0] {
	case 0:
		regenerateAllMediaFlow()
	case 1:
		removeAllMediaFlow()
	}
}

func regenerateAllMediaFlow() {
	msg := "Regenerate artwork for all shortcuts?\n\nThis will (re)create bg.png for every\nshortcut using the current Artwork mode."
	confirmed, err := gaba.ConfirmationMessage(msg,
		[]gaba.FooterHelpItem{
			{ButtonName: "B", HelpText: "Cancel"},
			{ButtonName: "A", HelpText: "Regenerate", IsConfirmButton: true},
		},
		gaba.MessageOptions{ConfirmButton: constants.VirtualButtonA},
	)
	if isErrCancelled(err) || confirmed == nil || !confirmed.Confirmed {
		return
	}

	settings := loadSettings()
	gaba.ProcessMessage("Regenerating artwork...",
		gaba.ProcessMessageOptions{ShowThemeBackground: true},
		func() (any, error) {
			return nil, regenerateAllMedia(settings)
		},
	)

	gaba.ConfirmationMessage(
		"Artwork regenerated for all shortcuts.",
		[]gaba.FooterHelpItem{
			{ButtonName: "A", HelpText: "OK", IsConfirmButton: true},
		},
		gaba.MessageOptions{ConfirmButton: constants.VirtualButtonA},
	)
}

func removeAllMediaFlow() {
	msg := "Remove all artwork?\n\nThis will delete bg.png from every\nshortcut's .media folder."
	confirmed, err := gaba.ConfirmationMessage(msg,
		[]gaba.FooterHelpItem{
			{ButtonName: "B", HelpText: "Cancel"},
			{ButtonName: "A", HelpText: "Remove", IsConfirmButton: true},
		},
		gaba.MessageOptions{ConfirmButton: constants.VirtualButtonA},
	)
	if isErrCancelled(err) || confirmed == nil || !confirmed.Confirmed {
		return
	}

	gaba.ProcessMessage("Removing artwork...",
		gaba.ProcessMessageOptions{ShowThemeBackground: true},
		func() (any, error) {
			return nil, removeAllMedia()
		},
	)

	gaba.ConfirmationMessage(
		"Artwork removed from all shortcuts.",
		[]gaba.FooterHelpItem{
			{ButtonName: "A", HelpText: "OK", IsConfirmButton: true},
		},
		gaba.MessageOptions{ConfirmButton: constants.VirtualButtonA},
	)
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
