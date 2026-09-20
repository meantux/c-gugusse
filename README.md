# Film capture software for the Gugusse Compact with the Raspberry Pi HQ camera

[![Latest release](https://img.shields.io/github/v/release/meantux/c-gugusse)](https://github.com/meantux/c-gugusse/releases/latest)
**[Download the latest .deb](https://github.com/meantux/c-gugusse/releases/latest)** - all versions on the [Releases page](https://github.com/meantux/c-gugusse/releases).

Capture software for the Gugusse Compact, a film scanner built around a Raspberry Pi and the **Raspberry Pi HQ camera** (Sony IMX477), driven through libcamera. Runs on a Raspberry Pi 4B (Raspberry Pi OS / Debian 13, 64-bit); the film is lit with white LEDs. Find the STL files and the instructions to build your own Gugusse Compact at [deniscarl.com](http://www.deniscarl.com) for free and without any subscriptions.

## Features

- Live colour preview, with a click-to-zoom 1:1 raw view for checking focus
- Histogram of the sensor's **raw 12-bit values** (0-4095, clipping in red)
- Single exposure setting, red/blue white-balance gains (no auto-exposure,
  no auto-white-balance), brightness / contrast / saturation
- Save as **DNG** (12-bit raw) or **JPG** (ISP-processed) - dropdown
- Motor-driven film transport sequence; finished frames leave by **FTP upload**
  or **local copy** (e.g. a USB drive), chosen in *Other settings...*

## Setup

The HQ camera is auto-detected by Raspberry Pi OS (`camera_auto_detect=1`,
the default in `/boot/firmware/config.txt`) - no extra configuration is
normally needed. Check with `rpicam-hello --list-cameras`. If the camera is
not listed, add `dtoverlay=imx477` under `[all]` in `/boot/firmware/config.txt`
and reboot.

## Install (Debian package)

Download `c-gugusse_<version>_arm64.deb` from the
[Releases page](https://github.com/meantux/c-gugusse/releases) (the newest is at
the top, under *Assets*), then on the Raspberry Pi (Raspberry Pi OS, Debian 13 trixie, 64-bit):

    sudo apt install ./c-gugusse_<version>_arm64.deb

`apt` pulls in all dependencies. The package adds the user who ran `sudo`
(or the first user, uid 1000) to the `gpio` and `video` groups - log out and
back in once. Other users: `sudo adduser <user> gpio && sudo adduser <user> video`.
Start it from the menu (*Graphics > Gugusse film scanner*) or run `c-gugusse`.

To build the package yourself, on the Pi: `./build-deb.sh` (result in `..`).
Publishing a release: `git tag v0.1.0 && git push --tags` - GitHub Actions
(`.github/workflows/deb.yml`) builds the `.deb` and attaches it to a Release.
Bump `debian/changelog` (and `VERSION` in `app/CMakeLists.txt`) first.

## Build from source (development)

Install the build/runtime dependencies (see `libraries.list`):

    sudo apt update
    sudo apt install -y $(grep -v '^\s*#' libraries.list | grep -v '^\s*$')

    cmake -S app -B app/build -DCMAKE_BUILD_TYPE=Release
    cmake --build app/build -j4
    ./app/build/c-gugusse

Run from the build tree, the app uses the repo's `assets/` and `defaults/`;
installed, it uses `/usr/share/c-gugusse/` (override with `C_GUGUSSE_DATA_DIR`).

## Configuration files

They live in `~/.config/c-gugusse/` (or `$XDG_CONFIG_HOME/c-gugusse/`). Any
file that is missing at startup is created from `defaults/` (installed as
`/usr/share/c-gugusse/defaults/`); existing files are never overwritten, and
they are kept when the package is upgraded or removed.

| File | Contents |
|---|---|
| `hq-camera-settings.json` | exposure, red/blue gain, brightness, contrast, saturation (written by *Save Settings*) |
| `preferences.json` | selected film format and DNG/JPG choice (written by *Save Settings*) |
| `hardwarecfg.json` | GPIO wiring, motor limits, film formats; also `saveMode` (`ftp`/`local`) and `localFilePath` (written by *Other settings...*, as are the motors' `invert` flags) |
| `ftp.json` | FTP server for uploads (written by *Other settings...*) |

Captured frames are written to `/dev/shm/<NNNNN>.dng|jpg`, then moved to
`/dev/shm/complete/`, from where they are either uploaded over FTP to
`<ftp path>/<project>/` or moved to `<localFilePath>/<project>/`, and removed
from `/dev/shm/complete/` once safely there.

## Other settings

The *Other settings...* button (bottom of the window, disabled while a sequence
runs) opens a window that replaces `../GugusseRoller`'s
`MotorsAndFtpSetup.py`:

- **Export mode** - *FTP upload* (server, user, password, path, and a *Test FTP
  settings* button that creates, uploads to and removes a scratch directory) or
  *Local copy* (a save path, typically the mount point of a USB drive; it must
  already exist - only the project sub-directory is created).
- **Motor direction** - a *Test motor* button per motor (turns it a short way
  "clockwise") and an *invert* checkbox to correct the direction.

*Save* writes `ftp.json` / `hardwarecfg.json` immediately and applies the
change without restarting.

## Frame numbering

Whenever the project name changes (and at startup, and when the export mode
changes) the destination `<project>/` directory - on the FTP server or the local
path - is listed, and numbering continues one past the highest frame number
found there (`00041.dng` -> next frame is 42). A new or empty project starts at
0. Frames still waiting in `/dev/shm/complete/` count too. Capturing is held
off for the moment the listing takes; if the destination can't be checked
(server unreachable, drive not mounted) a warning says so and numbering simply
continues from its current value.
