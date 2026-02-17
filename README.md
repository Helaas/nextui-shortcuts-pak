# Shortcuts

Create and manage NextUI main menu shortcuts for ROMs and Tools on tg5040/tg5050 devices. This Pak uses Gabagool for a native UI and builds the shortcut folders and .m3u files that NextUI auto-launches.

## Supported Platforms

| Platform | Device | Build |
|----------|--------|-------|
| `tg5040` | TrimUI Brick, TrimUI Smart Pro | Docker (ARM64) |
| `tg5050` | TrimUI Smart Pro S | Docker (ARM64) |

## What It Does

- Adds ROM shortcuts by creating a folder in `Roms/` with a matching `.m3u`
- Adds Tool shortcuts using a `SHORTCUT.pak` bridge emulator and marker file
- Lists and deletes existing shortcuts
- Auto-installs `SHORTCUT.pak` if it is missing

## Usage

1. Launch **Shortcuts** from the NextUI Tools menu
2. Pick one of:
   - **Add ROM Shortcut**
   - **Add Tool Shortcut**
   - **Manage Shortcuts**
3. Follow the on-screen prompts

## How Shortcuts Work

ROM shortcut structure:
```
/mnt/SDCARD/Roms/STAR_NAME (TAG)/
  STAR_NAME (TAG).m3u
```
The `.m3u` contains a relative path to the real ROM, for example:
```
../Sega Genesis (MD)/Battletoads (World).md
```

Tool shortcut structure:
```
/mnt/SDCARD/Roms/STAR_NAME (SHORTCUT)/
  STAR_NAME (SHORTCUT).m3u
  target
```
The `target` file contains the full path to the tool `.pak` directory. The `.m3u` contains the string `target`.

## Logging

Logs are written to:
```
/mnt/SDCARD/.userdata/<platform>/logs/shortcuts.log
```
The platform is read from `PLATFORM`. If not set, it defaults to `tg5040`.

## Building

### Prerequisites

**macOS (development):**
```bash
brew install go sdl2 sdl2_ttf sdl2_image sdl2_gfx
```

**Embedded (tg5040/tg5050):**
- Docker with ARM64 support

### First-Time Setup

```bash
make deps
```

This vendors dependencies and applies the Gabagool power button patch for tg5050.

### Build Commands

```bash
# Auto-detect platform and build
make

# Build for specific platform
make tg5040
make tg5050

# Build for all embedded platforms
make embedded

# Package as .pak bundles for NextUI
make package

# Export TrimUI .pakz (Tools/tg5040 + Tools/tg5050 layout)
make export-trimui

# Update dependencies and re-apply patches
make deps

# See all targets
make help
```

### Output

| Target | Output |
|--------|--------|
| tg5040 | `build/release/tg5040/Shortcuts.pak.zip` |
| tg5050 | `build/release/tg5050/Shortcuts.pak.zip` |
| export-trimui | `build/release/trimui/Shortcuts.pakz` |

The `.pak.zip` includes the binary, `launch.sh`, `pak.json`, `LICENSE`, and required shared libraries.

## Installing on a Handheld

1. Build and package: `make package` or `make export-trimui`
2. If using `make package`, extract `Shortcuts.pak.zip` to your SD card as `Tools/<platform>/Shortcuts.pak/`
3. If using `make export-trimui`, place `Shortcuts.pakz` in the root of your SD card; NextUI will auto-install it upon (re)boot
4. Launch from the NextUI Tools menu

## Acknowledgements

Built with [Gabagool](https://github.com/BrandonKowalski/gabagool) by [@BrandonKowalski](https://github.com/BrandonKowalski).

## License

MIT
