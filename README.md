# SimpleStreamer

## About
### Requires a stable & strong network connection, bitrates can exceed *250* Mbps

This repo is a proof of concept for display streaming from a jailbroken Switch 2 to another device. It uses the `caps2:s` service to create a 1080p MJPEG stream targeting 30fps and streams it to a client over TCP. It does not reach this target and it is unlikely it will in the near future.

This README is human-written, but the code & research behind this project are **significantly AI-assisted**.

Only tested on a v23.0.1 Switch 2 running kurumuz's ns2jb through SmileBASIC 4, and a Windows client.

## Features

* Streams MJPEG over TCP to a connected client
* 1920x1080 capture at up to 30fps
    * *See more in `Performance details` below*
* Two client options:
    * The cv2-client is multi-threaded, displays live FPS and bitrate stats in the window title, and includes a half-scale decode toggle.
        * *Toggled with* `H` *, half-scale decode reduces the client output to 960x540. Should improve playback performance if you are bottlenecked by decoding on the client side.*
    * The ffplay-client is a simple, lightweight alternative.
* Stream module configuration:
    * `stream-conf.nro` allows for the `stream-module` plugin to be stopped and for the network stream to be toggled.
        * *plugins cannot be restarted without re-running kaiHEN from a fresh boot*
* Client and host monitoring:
    * Both client players report the received framerate, frame size, and bitrate
    * `cv2-client.py` also the network FPS (frames received per second), display FPS (frames decoded per second) to identify any bottlenecks in your setup.
    * `stream-conf.nro` can show useful information about the `stream-module` plugin and its connection: plugin status, capture status, capture & transmission framerates, client connection status, console IP, etc...
* Switching apps and visiting the home menu are handled safely
    * The capture method used is not able to function on the home screen or during a game's black boot screen, but deals with these transitions safely to prevent system panics
* Works in both docked and handheld mode
    * Stream continues without issue when swapping between docked and handheld, and minimal performance is lost when streaming in handheld mode.

## Installation

### Switch 2 Installation
Make sure you are successfully able to boot into kaiHen before attempting installation.

1. Download `stream-conf.nro` and `stream-module.nso` from the latest release
2. Copy `stream-conf.nro` to `sdmc:/switch`
3. Copy `stream-module.nso` to `sdmc:/kaihen/plugins`, creating the `plugins` folder if needed
    * If updating or replacing `stream-module.nso`, delete `sdmc:/kaihen/plugin-packages/stream-module.nsp`. You currently must reboot for changes to take effect

### Client Installation

Download `cv2-client.py` or `ffplay-client.py` from the latest release

## Usage

### Switch 2 Usage
 
With the correct files on the SD card, the stream should be available as soon as HEN launch is completed or the homebrew menu has been opened. Plugin status can be checked with `stream-conf.nro`

### Client Usage
For `cv2-client.py`: 
1. Ensure `opencv-python` is installed
2. Run `python cv2-client.py <SWITCH2-IP>`
    * Press `h` to enable half-scale decoding. Useful if you are struggling to decode fast enough (Disp < Net in the stream stats) 
3. Press `Esc` or `q` to quit the stream.


For `ffplay-client.py`: 
1. Ensure `ffmpeg` is installed
2. Run `python ffplay-client.py <SWITCH2-IP>`
3. Press `Esc` or `q` to quit the stream.

## Building
Only tested on Windows.
Ensure you have installed [devkitPro](https://devkitpro.org/) and [ounce-devkit](https://github.com/ouncebrew/ounce-devkit) properly.

### MSYS2/Bash
Run `make` in `stream-conf/` and `stream-module/` to build the respective `.nro` and `.nso`

### Powershell
Run `.\build.ps1` in `stream-conf/` and `stream-module/` to build the respective `.nro` and `.nso`

## Performance Details

This plugin's capture loop opens the `caps2:s` service and dispatches `Cmd 101` in mode `0x00020200`. This command encodes a JPEG image with Quality (`q`) set to 100. This JPEG is seemingly only ever encoded with a smaller `q` if the encode fails due to the caller buffer being too small. 
Time isn't saved by forcing a smaller `q`, as for each frame it starts at 100 and has to be decremented by the failed encode attempts.
Additionally, `Cmd 101` hardcodes the resolution option as 1080p, which is why there are no problems swapping between docked and handheld. `Cmd 101` also does not open a continuous capture, so for every frame:
* a capture session is allocated,
* it waits for the display layer in order to actually capture the frame,
* the capture session is torn down again,
* the JPEG is then encoded on the cpu at full quality.

There's no pipelining, and capture seems to adhere to VSync, so it's not currently very feasible to hit the 30fps target whilst running a game. ExeFS/IPS patching support for system titles could help change these hardcoded values to ones that allow for the targeted framerate, but this is not currently a feature of kaiHEN, nor do I know if it could ever be.

## Credits

* **[kaiHEN](https://github.com/ouncebrew/kaihen)** (kaiHEN contributors, ISC License) — Switch 2 homebrew & background plugin runtime (`libkaihen_plugin` / `libkaihen`).
* **[libounce](https://github.com/ouncebrew/ounce-devkit)** (libounce contributors & libnx Authors, ISC License) — Switch 2 system service and kernel bindings (`libounce_core`).
* **[devkitPro](https://devkitpro.org/)** — `devkitA64` toolchain and `elf2nso` / `switch-tools`.
* **[ns2jb](https://gitlab.com/kurumuz/ns2jb)** (kurumuz) — Userland entrypoint used during testing.
* **[SwitchBrew](https://switchbrew.org/)** — Horizon OS service (`capsrv`, `bsd`, `vi`) documentation.
* **PC Client Dependencies:**
  * [OpenCV (`opencv-python`)](https://opencv.org/) (Apache 2.0) & [NumPy](https://numpy.org/) (BSD 3-Clause)
  * [FFmpeg / `ffplay`](https://ffmpeg.org/) (LGPL v2.1+ / GPL v2+)