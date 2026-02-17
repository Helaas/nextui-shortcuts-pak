package main

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// ── Device paths ─────────────────────────────────────────────

const (
	sdcardPath     = "/mnt/SDCARD"
	romsPath       = sdcardPath + "/Roms"
	toolsPath      = sdcardPath + "/Tools"
	emusUserPath   = sdcardPath + "/Emus"
	systemPaksPath = sdcardPath + "/.system"
)

// shortcutPrefix is the Unicode star character used to mark shortcuts.
const shortcutPrefix = "\u2605"

// bridgeEmuTag is the tag used for tool shortcuts.
const bridgeEmuTag = "SHORTCUT"

// ── Data types ───────────────────────────────────────────────

// ConsoleDir represents a ROM console directory.
type ConsoleDir struct {
	Name    string // e.g. "Sega Genesis (MD)"
	Tag     string // e.g. "MD"
	Path    string // full path to the directory
	Display string // display name without tag
}

// ROMFile represents a ROM file within a console directory.
type ROMFile struct {
	Name    string // filename, e.g. "Battletoads (World).md"
	Path    string // full path
	Display string // display name without extension
}

// ToolPak represents a tool .pak directory.
type ToolPak struct {
	Name    string // e.g. "SDLReader"
	Path    string // full path, e.g. "/mnt/SDCARD/Tools/tg5040/SDLReader.pak"
	Display string // display name
}

// Shortcut represents an existing shortcut on the device.
type Shortcut struct {
	Name       string // folder name, e.g. "★ Battletoads (MD)"
	Tag        string // e.g. "MD" or "SHORTCUT"
	Display    string // e.g. "★ Battletoads"
	Path       string // full path to shortcut folder
	IsTool     bool   // true if this is a tool shortcut
	TargetPath string // resolved target (ROM file path or tool .pak path)
}

// ── Scanning functions ───────────────────────────────────────

// getBasePaths returns the device paths, adjusted for macOS development.
func getBasePaths() (roms, tools, emus string) {
	if platform == PlatformMac {
		// Use a local mock directory structure for development
		cwd, _ := os.Getwd()
		mock := filepath.Join(cwd, "mock_sdcard")
		return filepath.Join(mock, "Roms"),
			filepath.Join(mock, "Tools", "tg5040"),
			filepath.Join(mock, "Emus", "tg5040")
	}
	return romsPath,
		filepath.Join(toolsPath, string(platform)),
		filepath.Join(emusUserPath, string(platform))
}

// scanConsoleDirs returns all ROM console directories (non-shortcut).
func scanConsoleDirs() ([]ConsoleDir, error) {
	romsDir, _, _ := getBasePaths()
	entries, err := os.ReadDir(romsDir)
	if err != nil {
		return nil, fmt.Errorf("reading roms dir: %w", err)
	}

	var consoles []ConsoleDir
	for _, e := range entries {
		if !e.IsDir() || isHidden(e.Name()) || isShortcutFolder(e.Name()) {
			continue
		}
		name := e.Name()
		tag := extractTag(name)
		if tag == "" {
			continue // no emu tag — skip
		}
		consoles = append(consoles, ConsoleDir{
			Name:    name,
			Tag:     tag,
			Path:    filepath.Join(romsDir, name),
			Display: extractDisplayName(name),
		})
	}

	sort.Slice(consoles, func(i, j int) bool {
		return strings.ToLower(consoles[i].Display) < strings.ToLower(consoles[j].Display)
	})
	return consoles, nil
}

// scanROMs returns all ROM files in a console directory.
func scanROMs(consoleDir string) ([]ROMFile, error) {
	entries, err := os.ReadDir(consoleDir)
	if err != nil {
		return nil, fmt.Errorf("reading rom dir: %w", err)
	}

	var roms []ROMFile
	for _, e := range entries {
		if e.IsDir() || isHidden(e.Name()) {
			continue
		}
		name := e.Name()
		roms = append(roms, ROMFile{
			Name:    name,
			Path:    filepath.Join(consoleDir, name),
			Display: stripExtension(name),
		})
	}

	sort.Slice(roms, func(i, j int) bool {
		return strings.ToLower(roms[i].Display) < strings.ToLower(roms[j].Display)
	})
	return roms, nil
}

// scanTools returns all tool .pak directories for the current platform.
func scanTools() ([]ToolPak, error) {
	_, toolsDir, _ := getBasePaths()
	entries, err := os.ReadDir(toolsDir)
	if err != nil {
		return nil, fmt.Errorf("reading tools dir: %w", err)
	}

	var tools []ToolPak
	for _, e := range entries {
		if !e.IsDir() || isHidden(e.Name()) {
			continue
		}
		name := e.Name()
		if !strings.HasSuffix(name, ".pak") {
			continue
		}
		displayName := strings.TrimSuffix(name, ".pak")
		tools = append(tools, ToolPak{
			Name:    displayName,
			Path:    filepath.Join(toolsDir, name),
			Display: displayName,
		})
	}

	sort.Slice(tools, func(i, j int) bool {
		return strings.ToLower(tools[i].Display) < strings.ToLower(tools[j].Display)
	})
	return tools, nil
}

// scanShortcuts returns all existing shortcuts.
func scanShortcuts() ([]Shortcut, error) {
	romsDir, _, _ := getBasePaths()
	entries, err := os.ReadDir(romsDir)
	if err != nil {
		return nil, fmt.Errorf("reading roms dir: %w", err)
	}

	var shortcuts []Shortcut
	for _, e := range entries {
		if !e.IsDir() || !isShortcutFolder(e.Name()) {
			continue
		}
		name := e.Name()
		tag := extractTag(name)
		isTool := tag == bridgeEmuTag
		sc := Shortcut{
			Name:    name,
			Tag:     tag,
			Display: extractDisplayName(name),
			Path:    filepath.Join(romsDir, name),
			IsTool:  isTool,
		}

		// Resolve target
		if isTool {
			targetFile := filepath.Join(sc.Path, "target")
			data, err := os.ReadFile(targetFile)
			if err == nil {
				sc.TargetPath = strings.TrimSpace(string(data))
			}
		} else {
			m3uFile := filepath.Join(sc.Path, name+".m3u")
			data, err := os.ReadFile(m3uFile)
			if err == nil {
				relPath := strings.TrimSpace(string(data))
				sc.TargetPath = filepath.Join(sc.Path, relPath)
			}
		}

		shortcuts = append(shortcuts, sc)
	}

	sort.Slice(shortcuts, func(i, j int) bool {
		return strings.ToLower(shortcuts[i].Display) < strings.ToLower(shortcuts[j].Display)
	})
	return shortcuts, nil
}

// ── Shortcut creation / removal ──────────────────────────────

// createROMShortcut creates a ROM shortcut folder with m3u.
func createROMShortcut(displayName, tag, consoleDirName, romFileName string) error {
	romsDir, _, _ := getBasePaths()
	folderName := fmt.Sprintf("%s %s (%s)", shortcutPrefix, displayName, tag)
	folderPath := filepath.Join(romsDir, folderName)

	if err := os.MkdirAll(folderPath, 0755); err != nil {
		return fmt.Errorf("creating shortcut dir: %w", err)
	}

	// The relative path from shortcut folder to the actual ROM
	relPath := fmt.Sprintf("../%s/%s", consoleDirName, romFileName)

	m3uPath := filepath.Join(folderPath, folderName+".m3u")
	if err := os.WriteFile(m3uPath, []byte(relPath), 0644); err != nil {
		return fmt.Errorf("writing m3u: %w", err)
	}

	return nil
}

// createToolShortcut creates a tool shortcut folder with m3u + target.
func createToolShortcut(displayName, pakPath string) error {
	romsDir, _, _ := getBasePaths()
	folderName := fmt.Sprintf("%s %s (%s)", shortcutPrefix, displayName, bridgeEmuTag)
	folderPath := filepath.Join(romsDir, folderName)

	if err := os.MkdirAll(folderPath, 0755); err != nil {
		return fmt.Errorf("creating shortcut dir: %w", err)
	}

	// Write target file containing the .pak path
	targetPath := filepath.Join(folderPath, "target")
	if err := os.WriteFile(targetPath, []byte(pakPath), 0644); err != nil {
		return fmt.Errorf("writing target: %w", err)
	}

	// Write m3u that points to "target"
	m3uPath := filepath.Join(folderPath, folderName+".m3u")
	if err := os.WriteFile(m3uPath, []byte("target"), 0644); err != nil {
		return fmt.Errorf("writing m3u: %w", err)
	}

	return nil
}

// removeShortcut removes a shortcut folder entirely.
func removeShortcut(shortcutPath string) error {
	return os.RemoveAll(shortcutPath)
}

// ── Bridge emu management ────────────────────────────────────

const bridgeLaunchScript = "#!/bin/sh\n# SHORTCUT.pak - Bridge emulator for tool shortcuts.\nTARGET=$(cat \"$1\")\nif [ -x \"$TARGET/launch.sh\" ]; then\n    exec \"$TARGET/launch.sh\"\nfi\n"

// ensureBridgeEmu makes sure SHORTCUT.pak exists for tool shortcuts.
func ensureBridgeEmu() {
	if platform == PlatformMac {
		return // not needed on macOS
	}

	_, _, emusDir := getBasePaths()
	pakDir := filepath.Join(emusDir, "SHORTCUT.pak")
	launchPath := filepath.Join(pakDir, "launch.sh")

	if _, err := os.Stat(launchPath); err == nil {
		return // already exists
	}

	if err := os.MkdirAll(pakDir, 0755); err != nil {
		logError("creating SHORTCUT.pak dir", err)
		return
	}

	if err := os.WriteFile(launchPath, []byte(bridgeLaunchScript), 0755); err != nil {
		logError("writing SHORTCUT.pak launch.sh", err)
	}
}

// ── String utilities ─────────────────────────────────────────

// isHidden checks if a name should be hidden (dotfiles, .disabled, map.txt).
func isHidden(name string) bool {
	return strings.HasPrefix(name, ".") ||
		strings.HasSuffix(name, ".disabled") ||
		name == "map.txt"
}

// isShortcutFolder checks if a folder name is a shortcut (starts with ★).
func isShortcutFolder(name string) bool {
	return strings.HasPrefix(name, shortcutPrefix)
}

// extractTag extracts the emulator tag from a directory name.
// e.g. "Game Boy Advance (GBA)" -> "GBA", "★ Battletoads (MD)" -> "MD"
func extractTag(name string) string {
	idx := strings.LastIndex(name, "(")
	if idx < 0 {
		return ""
	}
	end := strings.LastIndex(name, ")")
	if end <= idx {
		return ""
	}
	return strings.TrimSpace(name[idx+1 : end])
}

// extractDisplayName extracts the display name, stripping the trailing (TAG).
// e.g. "★ Battletoads (MD)" -> "★ Battletoads"
func extractDisplayName(name string) string {
	idx := strings.LastIndex(name, "(")
	if idx < 0 {
		return name
	}
	return strings.TrimSpace(name[:idx])
}

// stripExtension removes the file extension from a filename.
func stripExtension(name string) string {
	ext := filepath.Ext(name)
	if len(ext) >= 2 && len(ext) <= 5 {
		return strings.TrimSuffix(name, ext)
	}
	return name
}

// shortcutExists checks if a shortcut already exists for the given display name and tag.
func shortcutExists(displayName, tag string) bool {
	romsDir, _, _ := getBasePaths()
	folderName := fmt.Sprintf("%s %s (%s)", shortcutPrefix, displayName, tag)
	folderPath := filepath.Join(romsDir, folderName)
	_, err := os.Stat(folderPath)
	return err == nil
}
