# SHIELD Theme Music Extractor


Offline 3DS app to find, preview and copy the `BGM.ogg` audio from Anemone themes. It scans `sdmc:/themes/`, including ZIP archives, without unpacking whole archives.


## Features


- Lists each music file by its theme name and source type (ZIP or folder).
- Previews OGG Vorbis audio before copying; OGG playback is confirmed working on the project owner's 3DS.
- Copies only the selected `BGM.ogg`. The default output filename is the theme name plus `.ogg` to prevent collisions; you can choose `BGM.ogg` instead.
- Prompts before overwriting an existing file.
- Lets you choose the destination folder and lists folders containing MP3 files first.
- Works fully offline and does not modify the original theme.


## Controls


- Theme list: Up/Down selects, Y previews or stops, A starts the copy flow, X rescans, B stops preview.
- Destination picker: Up/Down selects, A opens a folder, X selects the current folder, B goes up/returns.
- Filename: A saves as `<theme name>.ogg`; X keeps `BGM.ogg`. Existing files require confirmation.
- First launch shows a 3-page guide. Hold SELECT at startup to view it again.


## Download


[Download ShieldThemeMusicExtractor.cia v1.0.0](https://github.com/Shield-77/Shield-Theme-Music-Extractor/releases/download/v1.0.0/ShieldThemeMusicExtractor.cia)


## Build and license


The complete source project, including Makefile, assets and third-party license notices, is available in [ShieldThemeMusicExtractor-Source-v1.0.0.zip](https://github.com/Shield-77/Shield-Theme-Music-Extractor/blob/main/ShieldThemeMusicExtractor-Source-v1.0.0.zip). Build with devkitPro and the dependencies listed in the archive's README. Application code and original artwork use the MIT License. Third-party library licenses are included separately in the source archive.


Please respect copyright and audio licensing. I developed this tool simply to allow for listening—on the 3DS—to the music found in many themes; these tracks often have great tunes but are frequently impossible to track down, even using Shazam (I mean, how could Shazam possibly analyze songs sung by Inklings? 😭). Anyway, the goal is to provide the simplest way possible to listen to the background music from your favorite themes—and perhaps even enjoy them via Shield DJ? nah listen theme anywhere in OGG format !
