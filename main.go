package main

import (
	"errors"
	"log"
	"os"
	"path/filepath"
	"runtime"
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
	platform = detectPlatform()

	logPath := getLogPath()
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
		case mainActionQuit:
			return
		}
	}
}

func detectPlatform() Platform {
	p := strings.ToUpper(os.Getenv("PLATFORM"))
	switch {
	case strings.Contains(p, "TG5050"):
		return PlatformTG5050
	case strings.Contains(p, "TG5040"), strings.Contains(p, "TG3040"):
		return PlatformTG5040
	default:
		if runtime.GOOS == "darwin" {
			return PlatformMac
		}
		return PlatformTG5040
	}
}

func getLogPath() string {
	if platform == PlatformMac {
		return filepath.Join(".", "shortcuts.log")
	}

	userdata := os.Getenv("SHARED_USERDATA_PATH")
	if userdata == "" {
		home := os.Getenv("HOME")
		if home == "" {
			home = "/root"
		}
		userdata = filepath.Join(home, ".userdata")
	}

	logDir := filepath.Join(userdata, string(platform), "logs")
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
