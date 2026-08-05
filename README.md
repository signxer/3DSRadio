# 3DSRadio - Internet Radio Player for Nintendo 3DS

A native homebrew internet radio client for the Nintendo 3DS, powered by [radio-browser.info](https://www.radio-browser.info/).

## Features

- Browse radio stations by genre/tag
- View top-clicked stations
- Search stations by name
- Play internet radio streams
- Now Playing screen with station info
- Full dual-screen interface

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

# Build the CIA file (optional, requires makerom/bannertool)
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
- **Format:** 3DSX / CIA

## Credits

- [radio-browser.info](https://www.radio-browser.info/) for the station database API
- [devkitPro](https://devkitpro.org/) for the 3DS homebrew toolchain
- [ClouDS-Music](https://github.com/cadl/ClouDS-Music) for architecture reference

## License

MIT
