# 3DSRadio - Internet Radio Player for Nintendo 3DS

A native homebrew internet radio client for the Nintendo 3DS, powered by [radio-browser.info](https://www.radio-browser.info/).

## Features

- Browse radio stations by genre/tag
- View top-clicked stations
- Search stations by name
- Play internet radio streams
- Now Playing screen with station info
- Full dual-screen interface
- Light and dark themes with saved preferences
- Touch search keyboard with QWERTY/symbol layouts and a paged Chinese pinyin candidate set
- MP3 and OGG/Vorbis playback with prebuffering, ICY stripping, and retry

## Prerequisites

- [devkitPro](https://devkitpro.org/) with devkitARM
- Install the following devkitPro packages:
  ```
  sudo dkp-pacman -S 3ds-dev 3ds-libcurl 3ds-mbedtls 3ds-libpng 3ds-libjpeg-turbo
  ```

## Building

```bash
# Clone the repository
git clone https://github.com/yourname/3DSRadio
cd 3DSRadio

# Download CA certificates for HTTPS
./scripts/get_cacert.sh

# Build the 3DSX file
make

# Build the CIA file (optional; bootstraps pinned makerom/bannertool)
make cia
```

## Usage

1. Copy `3DSRadio.3dsx` to your 3DS SD card's `/3ds/` folder
2. Launch via the Homebrew Launcher
3. Make sure WiFi is enabled
4. Browse genres, select a station, and enjoy!

## Controls

| Button | Action |
|--------|--------|
| D-Pad Up/Down | Navigate lists |
| A | Select / Play / Pause |
| B | Back |
| X | Volume down (playing) |
| Y | Volume up / Station info |

## Technical Details

- **Language:** C (C11)
- **Libraries:** citro2d, citro3d, libcurl, mbedtls, ctru
- **API:** radio-browser.info (open, free)
- **Audio:** ndsp (hardware audio)
- **Audio formats:** MP3 (minimp3), OGG/Vorbis (stb_vorbis); AAC is reported as unsupported unless an AAC backend is added to the toolchain
- **Buffering:** dedicated download/decode threads, prebuffer hysteresis, ICY metadata stripping, and six-wave weak-Wi-Fi preset
- **Format:** 3DSX / CIA

## Playback and troubleshooting

The player waits for a small prebuffer before starting and automatically retries
transient stream failures up to three times. The Settings screen can switch
between small, medium, and large buffers; use the large buffer on unstable WiFi.

Radio-browser lists stations in many formats. MP3 and OGG/Vorbis are playable in
the current build. AAC stations remain visible with an explicit unsupported
badge instead of entering a silent playback state.

Preferences are saved to `sdmc:/3ds/3DSRadio/settings.cfg` when the Settings
screen is changed.

Genres, languages, and the top-stations page are cached for the current
session to avoid repeating the same network request.

## Credits

- [radio-browser.info](https://www.radio-browser.info/) for the station database API
- [devkitPro](https://devkitpro.org/) for the 3DS homebrew toolchain
- [stb_vorbis](https://github.com/nothings/stb) for the public-domain OGG/Vorbis decoder
- [ClouDS-Music-FA](https://github.com/Epic0522/ClouDS-Music-FA) for the progressive playback, keyboard, candidate layout, and aero UI reference
- See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for reused component and asset attribution

## Build a CIA

The normal CI build produces both a 3DSX and an installable CIA.  CIA
packaging uses pinned host-side versions of `makerom` and `bannertool`, built
on demand under the ignored `.tools/` directory, so it does not depend on
whether the devkitPro package mirror currently exposes those tools.

With devkitARM configured and the CI-generated banner assets present, run:

```sh
make cia
```

The CIA tools are optional for regular 3DSX builds.  If the host has no CMake,
compiler, or network access, `make` and the 3DSX target remain independent;
only `make cia` requires the native packaging-tool bootstrap.

## License

MIT
