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
- Supports multi-disc games (subfolders containing a `.m3u` playlist)
- Supports single-disc CUE/BIN games (subfolders containing a `.cue` file)
- Lists and deletes existing shortcuts
- Auto-installs `SHORTCUT.pak` if it is missing
- Copies and composites artwork as a fullscreen `bg.png` for each shortcut (optional)
- Bulk-regenerates or removes artwork for all shortcuts at once

## Usage

1. Launch **Shortcuts** from the NextUI Tools menu
2. Pick one of:
   - **Add ROM Shortcut**
   - **Add Tool Shortcut**
   - **Manage Shortcuts**
   - **Manage Media**
   - **Settings**
3. Follow the on-screen prompts

## Menu Options

### Add ROM Shortcut

Browse your ROM library by console, pick a game, choose a sort position, and confirm. Supported game types:

| Label | Type | Detection |
|-------|------|-----------|
| _(none)_ | Single file ROM | `.md`, `.sfc`, `.nes`, … |
| `[Multi]` | Multi-disc game | Subfolder containing `{name}.m3u` |
| `[CUE]` | CUE/BIN disc image | Subfolder containing `{name}.cue` |

### Add Tool Shortcut

Browse installed Tools (`.pak` directories), pick one, choose a sort position, and confirm. A bridge emulator (`SHORTCUT.pak`) is installed automatically if missing.

### Manage Shortcuts

Browse all existing shortcuts. Select one to view details (name, type, tag, target path) and optionally delete it.

### Manage Media

Bulk artwork operations for all shortcuts:

| Option | Effect |
|--------|--------|
| **Regenerate all media** | Creates or replaces `bg.png` in every shortcut's `.media/` folder from its source artwork |
| **Remove all media** | Deletes `bg.png` (and the `.media/` folder if empty) from every shortcut |

### Settings

| Option | Values | Default |
|--------|--------|---------|
| Copy artwork when available | Off / On | Off |

When **Copy artwork** is enabled, creating a new shortcut will automatically generate a `bg.png` background image in the shortcut's `.media/` folder.

## Shortcut Position

When creating a shortcut you choose where it sorts in the NextUI main menu:

| Position | Folder prefix | Effect |
|----------|--------------|--------|
| **Bottom** (default) | `★ ` | Appears after Z |
| **Top** | `0) ` | Appears before A (NextUI strips the `0) ` from the display name) |
| **Alphabetical** | _(none)_ | Sorts with everything else by name |

## How Shortcuts Work

ROM shortcut structure:
```
/mnt/SDCARD/Roms/★ Name (TAG)/
  ★ Name (TAG).m3u        ← relative path to the real ROM
  .shortcut               ← clean display name (used by Shortcuts pak)
  .media/
    bg.png                ← generated fullscreen background (optional)
```

`.m3u` content by game type:

| Type | `.m3u` content |
|------|---------------|
| Single-file ROM | `../Console Dir (TAG)/game.rom` |
| Multi-disc | `../Console Dir (TAG)/GameName/GameName.m3u` |
| CUE/BIN folder | `../Console Dir (TAG)/GameName/GameName.cue` |

Tool shortcut structure:
```
/mnt/SDCARD/Roms/★ Name (SHORTCUT)/
  ★ Name (SHORTCUT).m3u  ← contains "target"
  target                  ← full path to the tool .pak directory
  .shortcut               ← clean display name
  .media/
    bg.png                ← generated fullscreen background (optional)
```

## Artwork / bg.png Generation

When artwork copying is enabled (or via **Manage Media → Regenerate all media**), the pak generates a 1280×720 `bg.png` for each shortcut:

1. **Base layer** — the device's global `/mnt/SDCARD/bg.png` scaled to cover the canvas (centre-cropped)
2. **Art layer** — the game/tool artwork scaled to fit within `55% × screen width` and `70% × screen height`, preserving aspect ratio, right-aligned and vertically centred, with rounded corners

Source artwork is looked up at:
- ROM shortcuts: `Roms/<Console Dir>/.media/<display name>.png`
- Tool shortcuts: `Tools/<platform>/.media/<display name>.png`

If no source artwork exists for a shortcut it is skipped silently.

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
