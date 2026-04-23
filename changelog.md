# Changelog: bugfixes vs main

This branch contains the non-hook fixes split out for the bugfix release. It
does not include the resume hook subsystem.

## User-facing changes

- Artwork background generation now uses the active NextUI theme background
  color instead of always using black for the "Art on Background color" mode.
- Shortcut artwork now also syncs a root menu thumbnail at
  `Roms/.media/<shortcut folder name>.png`, which is where NextUI looks for art
  when rendering root-level shortcut folders.
- Regenerating or removing shortcut media now also regenerates or removes those
  root menu thumbnails.
- Renaming or deleting a shortcut now keeps the root menu thumbnail in sync, so
  stale art does not remain under the old shortcut folder name.
- ROM scans now load per-console `map.txt` title mappings.
- `map.txt` is treated as a sidecar metadata file and is not shown as a ROM.
- ROM display titles can now come from `map.txt` while raw ROM stems are kept
  separately for shortcut targets and artwork lookup.
- Collection-style console folders keep their display tag, for example
  `Spelletjes (PS)` instead of only `Spelletjes`.
- Shortcut storage names now escape `/` and `%` in display titles while keeping
  the original visible title in `.shortcut`.
- Shortcut creation for mapped or renamed ROMs now writes `.m3u` targets using
  the raw ROM path, not the mapped display title.
- Duplicate shortcut detection now compares resolved ROM target paths instead
  of display name and tag text.
- The ROM picker marks ROMs that already have shortcuts with a trailing
  checkmark.
- Creating a second shortcut for the same ROM is blocked even if the existing
  shortcut has been renamed or the display title changed.
- Shortcut confirmation and success messages show the visible shortcut label
  instead of leaking encoded storage names.

## Build and tooling changes

- `.cache/` is ignored for generated local preview assets.
- `make mac` now initializes the Apostrophe submodule if needed and prepares the
  NextUI preview cache before building.
- Added `make setup-nextui-preview-cache` and
  `make clean-nextui-preview-cache`.
- Added `make update-apostrophe` to pin the Apostrophe submodule to
  `origin/main`.
- Advanced the repo-tracked Apostrophe gitlink to `8f8b610`.

## Internal API changes

- Added `sc_color` and changed `generate_artwork_bg()` to accept an explicit
  background color.
- Added `get_theme_bg_color()` to read NextUI theme color data via
  `nextval.elf`, falling back safely when unavailable.
- Added `rom_file.source_stem` so raw ROM stems are available after display
  title mapping.
- Added `build_rom_target_path()` and `rom_matches_shortcut_target()` for
  target-path based matching.
- Simplified `create_rom_shortcut()` so callers pass the selected `rom_file`
  directly and no longer pass a separate console directory name.
- Added shortcut thumbnail helpers for syncing and removing root menu
  thumbnails.

## Tests

- Added native tests for collection console labels preserving tags.
- Added native tests for `map.txt` title mapping, sidecar filtering, raw
  `source_stem` preservation, multi-disc entries, and CUE folder entries.
- Added native tests for mapped shortcut creation preserving raw targets and
  artwork lookup.
- Added native tests for slash-safe shortcut storage names while preserving
  visible `.shortcut` titles.
- Added native tests for root menu thumbnail creation and thumbnail rename
  behavior.
- Added native tests for target-path duplicate matching after shortcut rename.
- Expanded artwork test stubs so tests can assert generated artwork source and
  destination paths.
