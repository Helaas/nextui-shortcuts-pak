package main

import (
	"encoding/json"
	"fmt"
	"image"
	"image/png"
	"log"
	"os"
	"path/filepath"
	"sort"
	"strings"

	xdraw "golang.org/x/image/draw"
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

// ShortcutPosition controls where a shortcut appears in the file listing.
type ShortcutPosition int

const (
	// ShortcutPositionBottom places shortcuts after Z (uses ★ prefix, current default).
	ShortcutPositionBottom ShortcutPosition = iota
	// ShortcutPositionTop places shortcuts before A (uses "0) " prefix;
	// NextUI's trimSortingMeta strips "0) " so the game name displays cleanly).
	ShortcutPositionTop
	// ShortcutPositionAlpha sorts shortcuts alphabetically with everything else (no prefix).
	ShortcutPositionAlpha
)

// topPrefix is the sort prefix for top-of-list shortcuts.
// NextUI's trimSortingMeta strips "{digits}) " from display names, so "0) Foo" shows as "Foo".
const topPrefix = "0) "

// shortcutMarkerFile is the hidden file written inside every new shortcut folder.
// Its presence identifies shortcuts that have no ★ prefix (Top/Alpha positions),
// and its content is the clean display name (e.g. "Battletoads (World)").
const shortcutMarkerFile = ".shortcut"

// ── Data types ───────────────────────────────────────────────

// ConsoleDir represents a ROM console directory.
type ConsoleDir struct {
	Name    string // e.g. "Sega Genesis (MD)"
	Tag     string // e.g. "MD"
	Path    string // full path to the directory
	Display string // display name without tag
}

// ROMFile represents a ROM file or game folder within a console directory.
type ROMFile struct {
	Name        string // filename (e.g. "Battletoads (World).md") or dir name for folder-based games
	Path        string // full path
	Display     string // display name without extension
	IsMultiDisc bool   // true if this is a multi-disc folder (subdir containing {name}.m3u)
	IsCueFolder bool   // true if this is a single-disc folder (subdir containing {name}.cue)
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
		if !e.IsDir() || isHidden(e.Name()) || isShortcutFolder(filepath.Join(romsDir, e.Name())) {
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
	log.Printf("scanConsoleDirs: found %d console folders", len(consoles))
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
		if isHidden(e.Name()) {
			continue
		}
		name := e.Name()
		if e.IsDir() {
			dirPath := filepath.Join(consoleDir, name)
			// Multi-disc game: subfolder contains {name}.m3u playlist.
			if _, err := os.Stat(filepath.Join(dirPath, name+".m3u")); err == nil {
				roms = append(roms, ROMFile{
					Name:        name,
					Path:        dirPath,
					Display:     name,
					IsMultiDisc: true,
				})
			} else if _, err := os.Stat(filepath.Join(dirPath, name+".cue")); err == nil {
				// Single-disc CUE/BIN game: subfolder contains {name}.cue.
				roms = append(roms, ROMFile{
					Name:        name,
					Path:        dirPath,
					Display:     name,
					IsCueFolder: true,
				})
			}
			continue
		}
		roms = append(roms, ROMFile{
			Name:    name,
			Path:    filepath.Join(consoleDir, name),
			Display: stripExtension(name),
		})
	}

	sort.Slice(roms, func(i, j int) bool {
		return strings.ToLower(roms[i].Display) < strings.ToLower(roms[j].Display)
	})
	log.Printf("scanROMs: dir=%s roms=%d", consoleDir, len(roms))
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
	log.Printf("scanTools: dir=%s tools=%d", toolsDir, len(tools))
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
		if !e.IsDir() {
			continue
		}
		fullPath := filepath.Join(romsDir, e.Name())
		if !isShortcutFolder(fullPath) {
			continue
		}
		name := e.Name()
		tag := extractTag(name)
		isTool := tag == bridgeEmuTag

		// Read display name from marker file if present; fall back to extracting from folder name.
		display := readShortcutMarker(fullPath)
		if display == "" {
			display = extractDisplayName(name)
			// Strip ★ prefix from legacy shortcuts for a clean display name.
			display = strings.TrimPrefix(display, shortcutPrefix+" ")
		}

		sc := Shortcut{
			Name:    name,
			Tag:     tag,
			Display: display,
			Path:    fullPath,
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
	log.Printf("scanShortcuts: dir=%s shortcuts=%d", romsDir, len(shortcuts))
	return shortcuts, nil
}

// ── Shortcut creation / removal ──────────────────────────────

// createROMShortcut creates a ROM shortcut folder with m3u and a .shortcut marker.
// For multi-disc ROMs the m3u points to the playlist inside the game subfolder.
// For CUE folder ROMs the m3u points to the .cue file inside the game subfolder.
func createROMShortcut(displayName, tag, consoleDirName string, rom ROMFile, pos ShortcutPosition, settings AppSettings) error {
	romsDir, _, _ := getBasePaths()
	folderName := buildFolderName(pos, displayName, tag)
	folderPath := filepath.Join(romsDir, folderName)
	log.Printf("createROMShortcut: name=%s tag=%s rom=%s pos=%d multiDisc=%v", displayName, tag, rom.Name, pos, rom.IsMultiDisc)

	if err := os.MkdirAll(folderPath, 0755); err != nil {
		return fmt.Errorf("creating shortcut dir: %w", err)
	}

	// The relative path from the shortcut folder to the target
	var relPath string
	switch {
	case rom.IsMultiDisc:
		relPath = fmt.Sprintf("../%s/%s/%s.m3u", consoleDirName, rom.Name, rom.Name)
	case rom.IsCueFolder:
		relPath = fmt.Sprintf("../%s/%s/%s.cue", consoleDirName, rom.Name, rom.Name)
	default:
		relPath = fmt.Sprintf("../%s/%s", consoleDirName, rom.Name)
	}

	m3uPath := filepath.Join(folderPath, folderName+".m3u")
	if err := os.WriteFile(m3uPath, []byte(relPath), 0644); err != nil {
		return fmt.Errorf("writing m3u: %w", err)
	}

	if err := writeShortcutMarker(folderPath, displayName); err != nil {
		log.Printf("createROMShortcut: warning: could not write marker: %v", err)
	}

	if settings.CopyArtwork {
		artworkSrc := filepath.Join(romsDir, consoleDirName, ".media", displayName+".png")
		generateArtworkBg(artworkSrc, folderPath)
	}

	log.Printf("createROMShortcut: created folder=%s", folderPath)
	return nil
}

// createToolShortcut creates a tool shortcut folder with m3u, target, and a .shortcut marker.
func createToolShortcut(displayName, pakPath string, pos ShortcutPosition, settings AppSettings) error {
	romsDir, toolsDir, _ := getBasePaths()
	folderName := buildFolderName(pos, displayName, bridgeEmuTag)
	folderPath := filepath.Join(romsDir, folderName)
	log.Printf("createToolShortcut: name=%s pak=%s pos=%d", displayName, pakPath, pos)

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

	if err := writeShortcutMarker(folderPath, displayName); err != nil {
		log.Printf("createToolShortcut: warning: could not write marker: %v", err)
	}

	if settings.CopyArtwork {
		artworkSrc := filepath.Join(toolsDir, ".media", displayName+".png")
		generateArtworkBg(artworkSrc, folderPath)
	}

	log.Printf("createToolShortcut: created folder=%s", folderPath)
	return nil
}

// removeShortcut removes a shortcut folder entirely.
func removeShortcut(shortcutPath string) error {
	log.Printf("removeShortcut: path=%s", shortcutPath)
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
		log.Printf("ensureBridgeEmu: already present at %s", launchPath)
		return // already exists
	}

	if err := os.MkdirAll(pakDir, 0755); err != nil {
		logError("creating SHORTCUT.pak dir", err)
		return
	}

	if err := os.WriteFile(launchPath, []byte(bridgeLaunchScript), 0755); err != nil {
		logError("writing SHORTCUT.pak launch.sh", err)
		return
	}

	log.Printf("ensureBridgeEmu: created at %s", launchPath)
}

// ── String utilities ─────────────────────────────────────────

// isHidden checks if a name should be hidden (dotfiles, .disabled, map.txt).
func isHidden(name string) bool {
	return strings.HasPrefix(name, ".") ||
		strings.HasSuffix(name, ".disabled") ||
		name == "map.txt"
}

// isShortcutFolder checks if a full folder path is a shortcut.
// Detects: ★-prefixed folders (legacy + Bottom position) and folders with a .shortcut marker file (Top/Alpha).
func isShortcutFolder(folderPath string) bool {
	name := filepath.Base(folderPath)
	if strings.HasPrefix(name, shortcutPrefix) {
		return true
	}
	_, err := os.Stat(filepath.Join(folderPath, shortcutMarkerFile))
	return err == nil
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

// buildFolderName constructs the shortcut folder name for the given position.
//
//	Bottom: "★ Battletoads (World) (MD)"
//	Top:    "0) Battletoads (World) (MD)"
//	Alpha:  "Battletoads (World) (MD)"
func buildFolderName(pos ShortcutPosition, displayName, tag string) string {
	base := fmt.Sprintf("%s (%s)", displayName, tag)
	switch pos {
	case ShortcutPositionTop:
		return topPrefix + base
	case ShortcutPositionAlpha:
		return base
	default: // ShortcutPositionBottom
		return fmt.Sprintf("%s %s", shortcutPrefix, base)
	}
}

// generateArtworkBg composites a fullscreen bg.png for a shortcut's .media/ folder.
// It renders the device's global /mnt/SDCARD/bg.png as the base, then overlays artSrcPath
// scaled to match NextUI's thumbnail dimensions (screen_w*0.45 × screen_h*0.60, right-aligned,
// vertically centred at 50% screen height — identical to SCREEN_GAMELIST thumbnail rendering).
// Does nothing if artSrcPath does not exist.
func generateArtworkBg(artSrcPath, destFolder string) {
	if _, err := os.Stat(artSrcPath); err != nil {
		return // no art — skip silently
	}
	artImg, err := loadPNGImage(artSrcPath)
	if err != nil {
		log.Printf("generateArtworkBg: load art: %v", err)
		return
	}

	screenW, screenH := screenDimensions()
	canvas := image.NewNRGBA(image.Rect(0, 0, screenW, screenH))

	// Fill with black (fallback when global bg.png is absent or doesn't cover).
	pix := canvas.Pix
	for i := 0; i < len(pix); i += 4 {
		pix[i], pix[i+1], pix[i+2], pix[i+3] = 0x00, 0x00, 0x00, 0xff
	}

	// Layer 1: global bg.png scaled to cover the canvas (centre-crop, no letterbox).
	bgPath := globalBgPath()
	if bgImg, err := loadPNGImage(bgPath); err == nil {
		srcW, srcH := bgImg.Bounds().Dx(), bgImg.Bounds().Dy()
		scaleX := float64(screenW) / float64(srcW)
		scaleY := float64(screenH) / float64(srcH)
		scale := scaleX
		if scaleY > scaleX {
			scale = scaleY
		}
		newW := int(float64(srcW) * scale)
		newH := int(float64(srcH) * scale)
		scaledBg := image.NewNRGBA(image.Rect(0, 0, newW, newH))
		xdraw.BiLinear.Scale(scaledBg, scaledBg.Bounds(), bgImg, bgImg.Bounds(), xdraw.Src, nil)
		// Centre-crop: offset into scaledBg so the canvas window is centred.
		offX := (newW - screenW) / 2
		offY := (newH - screenH) / 2
		xdraw.Draw(canvas, canvas.Bounds(), scaledBg, image.Point{offX, offY}, xdraw.Src)
	}

	// Layer 2: game/tool art, right-aligned and vertically centred.
	//   max_w = screen_w * 0.55  (keep art within right ~55% so left-side menu text is clear)
	//   max_h = screen_h * 0.70  (taller than the game-list thumbnail so portrait art is wider)
	//   target_x = screen_w - new_w - right_margin (clamped ≥ 0)
	//   center_y = screen_h*0.50 - new_h/2
	maxW := int(float64(screenW) * 0.55)
	maxH := int(float64(screenH) * 0.70)
	artW, artH := thumbnailFit(artImg.Bounds().Dx(), artImg.Bounds().Dy(), maxW, maxH)
	scaledArt := image.NewNRGBA(image.Rect(0, 0, artW, artH))
	xdraw.BiLinear.Scale(scaledArt, scaledArt.Bounds(), artImg, artImg.Bounds(), xdraw.Over, nil)
	// Rounded corners: effective radius = FIXED_SCALE(2) * CFG_DEFAULT_THUMBRADIUS(20) = 40 px.
	// Mirrors GFX_ApplyRoundedCorners_8888 in nextui: pixels where dx²+dy²>r² become transparent.
	applyRoundedCorners(scaledArt, 40)

	const rightMargin = 30 // SCALE1(BUTTON_MARGIN * 3) at FIXED_SCALE=2
	targetX := max(0, screenW-artW-rightMargin)
	centerY := screenH/2 - artH/2
	artDst := image.Rect(targetX, centerY, targetX+artW, centerY+artH)
	xdraw.Draw(canvas, artDst, scaledArt, image.Point{}, xdraw.Over)

	// Save composite.
	mediaDir := filepath.Join(destFolder, ".media")
	if err := os.MkdirAll(mediaDir, 0755); err != nil {
		log.Printf("generateArtworkBg: mkdir .media: %v", err)
		return
	}
	f, err := os.Create(filepath.Join(mediaDir, "bg.png"))
	if err != nil {
		log.Printf("generateArtworkBg: create bg.png: %v", err)
		return
	}
	defer f.Close()
	if err := png.Encode(f, canvas); err != nil {
		log.Printf("generateArtworkBg: encode: %v", err)
	}
	log.Printf("generateArtworkBg: %s/.media/bg.png (%dx%d art=%dx%d)", destFolder, screenW, screenH, artW, artH)
}

// screenDimensions returns the native screen size for the current platform.
// TG5040 Smart Pro and TG5050 are both 1280×720. The TG5040 Brick is 1024×768 but
// cannot be detected at runtime; NextUI scales bg.png with aspect-ratio preservation.
func screenDimensions() (int, int) {
	return 1280, 720
}

// globalBgPath returns the path to the device's global background image.
func globalBgPath() string {
	sdcard := os.Getenv("SDCARD_PATH")
	if sdcard == "" {
		if platform == PlatformMac {
			cwd, _ := os.Getwd()
			return filepath.Join(cwd, "mock_sdcard", "bg.png")
		}
		return "/mnt/SDCARD/bg.png"
	}
	return filepath.Join(sdcard, "bg.png")
}

// loadPNGImage opens and decodes a PNG file.
func loadPNGImage(path string) (image.Image, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	return png.Decode(f)
}

// applyRoundedCorners sets pixels outside the corner arcs to fully transparent.
// Ports NextUI's GFX_ApplyRoundedCorners_8888: for each corner, pixels where
// dx²+dy² > radius² (dx/dy = distance past the corner edge) are zeroed.
func applyRoundedCorners(img *image.NRGBA, radius int) {
	b := img.Bounds()
	w, h := b.Dx(), b.Dy()
	if radius <= 0 || w == 0 || h == 0 {
		return
	}
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			dx := 0
			if x < radius {
				dx = radius - x
			} else if x >= w-radius {
				dx = x - (w - radius - 1)
			}
			dy := 0
			if y < radius {
				dy = radius - y
			} else if y >= h-radius {
				dy = y - (h - radius - 1)
			}
			if dx*dx+dy*dy > radius*radius {
				off := img.PixOffset(x, y)
				img.Pix[off], img.Pix[off+1], img.Pix[off+2], img.Pix[off+3] = 0, 0, 0, 0
			}
		}
	}
}

// thumbnailFit scales (srcW, srcH) to fit within (maxW, maxH) preserving aspect ratio.
func thumbnailFit(srcW, srcH, maxW, maxH int) (int, int) {
	if srcW == 0 || srcH == 0 {
		return maxW, maxH
	}
	newW, newH := maxW, srcH*maxW/srcW
	if newH > maxH {
		newH = maxH
		newW = srcW * maxH / srcH
	}
	return newW, newH
}

// shortcutArtSrcPath returns the source artwork PNG path for a shortcut.
// For tool shortcuts it looks in toolsDir/.media/; for ROM shortcuts it reads
// the shortcut's .m3u to determine which console folder owns the artwork.
func shortcutArtSrcPath(sc Shortcut) string {
	romsDir, toolsDir, _ := getBasePaths()
	if sc.IsTool {
		return filepath.Join(toolsDir, ".media", sc.Display+".png")
	}
	// Read the m3u inside the shortcut folder to find the console directory.
	m3uPath := filepath.Join(sc.Path, sc.Name+".m3u")
	data, err := os.ReadFile(m3uPath)
	if err != nil {
		return ""
	}
	relPath := strings.TrimSpace(string(data))
	// relPath is "../Console Dir (TAG)/game.rom" — second component is the console dir.
	parts := strings.SplitN(relPath, "/", 3)
	if len(parts) < 2 || parts[0] != ".." {
		return ""
	}
	consoleDirName := parts[1]
	return filepath.Join(romsDir, consoleDirName, ".media", sc.Display+".png")
}

// regenerateAllMedia regenerates bg.png for every existing shortcut that has
// source artwork available, creating .media/ if needed.
func regenerateAllMedia() error {
	shortcuts, err := scanShortcuts()
	if err != nil {
		return fmt.Errorf("scanning shortcuts: %w", err)
	}
	for _, sc := range shortcuts {
		artSrc := shortcutArtSrcPath(sc)
		if artSrc != "" {
			generateArtworkBg(artSrc, sc.Path)
		}
	}
	log.Printf("regenerateAllMedia: processed %d shortcuts", len(shortcuts))
	return nil
}

// removeAllMedia removes .media/bg.png from every existing shortcut.
func removeAllMedia() error {
	shortcuts, err := scanShortcuts()
	if err != nil {
		return fmt.Errorf("scanning shortcuts: %w", err)
	}
	for _, sc := range shortcuts {
		bgPath := filepath.Join(sc.Path, ".media", "bg.png")
		if err := os.Remove(bgPath); err != nil && !os.IsNotExist(err) {
			log.Printf("removeAllMedia: remove %s: %v", bgPath, err)
		}
		// Remove .media dir if it is now empty.
		_ = os.Remove(filepath.Join(sc.Path, ".media"))
	}
	log.Printf("removeAllMedia: processed %d shortcuts", len(shortcuts))
	return nil
}

// ── App settings ─────────────────────────────────────────────

// AppSettings holds persistent user preferences.
type AppSettings struct {
	CopyArtwork bool `json:"copy_artwork"`
}

// getSettingsPath returns the path to the settings JSON file.
func getSettingsPath() string {
	sdcard := os.Getenv("SDCARD_PATH")
	if sdcard == "" {
		if platform == PlatformMac {
			cwd, _ := os.Getwd()
			sdcard = filepath.Join(cwd, "mock_sdcard")
		} else {
			sdcard = "/mnt/SDCARD"
		}
	}
	return filepath.Join(sdcard, ".userdata", string(platform), "shortcuts_settings.json")
}

// loadSettings reads settings from disk. Returns defaults on any error (missing file, parse error).
func loadSettings() AppSettings {
	defaults := AppSettings{CopyArtwork: false}
	data, err := os.ReadFile(getSettingsPath())
	if err != nil {
		return defaults
	}
	var s AppSettings
	if err := json.Unmarshal(data, &s); err != nil {
		log.Printf("loadSettings: parse error: %v", err)
		return defaults
	}
	return s
}

// saveSettings persists settings to disk.
func saveSettings(s AppSettings) error {
	path := getSettingsPath()
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		return fmt.Errorf("creating settings dir: %w", err)
	}
	data, err := json.Marshal(s)
	if err != nil {
		return fmt.Errorf("marshalling settings: %w", err)
	}
	return os.WriteFile(path, data, 0644)
}

// readShortcutMarker reads the clean display name stored in the .shortcut marker file.
// Returns "" if the file does not exist or cannot be read.
func readShortcutMarker(folderPath string) string {
	data, err := os.ReadFile(filepath.Join(folderPath, shortcutMarkerFile))
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(data))
}

// writeShortcutMarker writes the clean display name to the .shortcut marker file
// inside the given shortcut folder.
func writeShortcutMarker(folderPath, displayName string) error {
	markerPath := filepath.Join(folderPath, shortcutMarkerFile)
	return os.WriteFile(markerPath, []byte(displayName), 0644)
}

// shortcutExists checks if a shortcut already exists for the given display name and tag
// under any of the three position prefixes.
func shortcutExists(displayName, tag string) bool {
	romsDir, _, _ := getBasePaths()
	for _, pos := range []ShortcutPosition{ShortcutPositionBottom, ShortcutPositionTop, ShortcutPositionAlpha} {
		folderPath := filepath.Join(romsDir, buildFolderName(pos, displayName, tag))
		if _, err := os.Stat(folderPath); err == nil {
			return true
		}
	}
	return false
}
