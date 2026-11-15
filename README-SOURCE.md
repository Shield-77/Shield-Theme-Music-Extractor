# SHIELD Theme Music

SHIELD Theme Music is an offline Old 3DS homebrew that finds `BGM.ogg` in Anemone themes, lets you listen before copying, and never changes the original theme.

## What it does

- Scans `sdmc:/themes/` and its subfolders for `BGM.ogg` files.
- Reads ZIP archives in place and copies only the selected audio entry.
- Shows the source theme name and whether it came from a ZIP or folder.
- Previews OGG Vorbis audio on the console.
- Preloads ZIP-contained OGG files up to 8 MiB in memory for a quicker preview start; larger files use the SD temporary-file fallback.
- Lets you choose a destination folder. Folders containing MP3 files appear first to make SHIELD DJ destinations easier to find.
- Names the copy after its theme by default. You can keep the generic `BGM.ogg` name instead.
- Asks before replacing an existing file.
- Works offline; it does not use Wi-Fi.
- Shows an animated loading screen while the theme library is scanned in the background.
- Shows a persistent, explicit copy result on both screens, including the file name and destination.
- Allocates large copy and ZIP extraction buffers on the heap to avoid overflowing the Old 3DS main-thread stack during copies.
- Shows the current destination folder as an explicit selection, with bounded directory checks to keep browsing responsive on the Old 3DS.
- Prevents copying a folder-based `BGM.ogg` onto itself and truncating the original.

## Controls

### Theme list

- Up / Down: select a theme.
- Y: start or stop its audio preview.
- A: choose a destination folder.
- B: stop preview.
- X: rescan `sdmc:/themes/`.
- START: quit.

### Destination picker

- Up / Down: select a folder. Folders with MP3 files are listed first.
- The highlighted first row represents the current folder.
- A: open the highlighted subfolder.
- X: choose the highlighted folder as the destination.
- B: go to the parent folder, or return to the theme list at the SD root.

### Copy name

- A: use `<theme name>.ogg`.
- X: use `BGM.ogg`.
- B: return to the destination picker.
- If that name already exists, the app asks before replacing it.

The first launch shows a three-page guide. Hold SELECT while launching the app to show it again. Press A to continue and B to go back.

## Compatibility

The output remains an OGG Vorbis file, suitable for SHIELD DJ Lite and Standard. OGG playback on the user's 3DS has been confirmed by the project owner.

## Build

Requires devkitPro with the 3DS toolchain, Citro2D/Citro3D, `3ds-libarchive`, `3ds-libvorbisidec`, `3ds-libogg`, and their compression dependencies. From this directory, run `make` for a 3DSX or `make cia` for an installable CIA. The CIA build uses `makerom` and `bannertool`. On this development setup their default paths are `../FourthTube-main/resource/tools/`; override `MAKEROM_WIN` and `BANNERTOOL_WIN` if yours differ.

The application code and original artwork are licensed under MIT; see [LICENSE](LICENSE). Third-party libraries retain their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and `licenses/`.



