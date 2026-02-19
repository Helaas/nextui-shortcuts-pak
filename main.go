package main

import (
	"errors"
	"log"
	"os"
	"path/filepath"
	"strings"

	_ "github.com/BrandonKowalski/certifiable"
	gaba "github.com/BrandonKowalski/gabagool/v2/pkg/gabagool"
)

// Platform represents the target device.
type Platform string

const (
	PlatformMac    Platform = "mac"
	PlatformTG5040 Platform = "tg5040"
	PlatformTG5050 Platform = "tg5050"
)

var platform Platform

func main() {
	platform = PlatformTG5040
	platformEnv := strings.ToUpper(os.Getenv("PLATFORM"))
	if strings.Contains(platformEnv, "TG5050") {
		platform = PlatformTG5050
	} else if strings.Contains(platformEnv, "TG5040") || strings.Contains(platformEnv, "TG3040") {
		platform = PlatformTG5040
	}

	log.SetFlags(log.LstdFlags | log.Lmicroseconds)
	log.SetPrefix("shortcuts: ")

	logPath := getLogPath()
	log.Printf("startup: platform=%s logPath=%s", platform, logPath)
	gaba.Init(gaba.Options{
		WindowTitle:    "Shortcuts",
		ShowBackground: true,
		LogPath:        logPath,
		IsNextUI:       platform != PlatformMac,
	})
	defer gaba.Close()

	ensureBridgeEmu()
	runApp()
}

func runApp() {
	for {
		action := showMainMenu()
		switch action {
		case mainActionAddROM:
			addROMShortcutFlow()
		case mainActionAddTool:
			addToolShortcutFlow()
		case mainActionManage:
			manageShortcutsFlow()
		case mainActionSettings:
			showSettingsScreen()
		case mainActionQuit:
			return
		}
	}
}

func getLogPath() string {
	sdcard := os.Getenv("SDCARD_PATH")
	if sdcard == "" {
		sdcard = "/mnt/SDCARD"
	}

	logDir := filepath.Join(sdcard, ".userdata", string(platform), "logs")
	return filepath.Join(logDir, "shortcuts.log")
}

// isErrCancelled checks if the error is a Gabagool user-cancelled error.
func isErrCancelled(err error) bool {
	return errors.Is(err, gaba.ErrCancelled)
}

// logError logs a non-nil, non-cancelled error.
func logError(context string, err error) {
	if err != nil && !isErrCancelled(err) {
		log.Printf("%s: %v", context, err)
	}
}
