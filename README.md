# SimpleStreamer

## About
### Requires USB 2.0 connectivity or a stable & strong network, bitrates can exceed 250 Mbps

This repo is a proof of concept for display streaming from a jailbroken Switch 2 to another device. It uses the `caps2:s` service to create a 1080p MJPEG stream targeting 30fps, alongside `auddmg:u` and `audsmx:u` for 48kHz 16-bit stereo audio, and streams both to a client over TCP or USB. It does not reach the 30fps target and it is unlikely it will in the near future.

This README is human-written, but the code & research behind this project are **significantly AI-assisted**.

Only tested on:
* a v23.0.1 Switch 2 running kurumuz's ns2jb through SmileBASIC 4
* the same Switch 2 running rushinz's seedsow-semi-untethered
* a Windows client

## Features

* Streams MJPEG and stereo audio to a connected client
    * 1920x1080 capture at up to 30fps
        * Real-world performance ranges from 15-20fps
        * *See more in `Performance details` below*
    * 48kHz & 16-bit, with volume controls
* Connection via USB or TCP
  * A client can connect wirelessly to the Switch 2 using TCP, this supports seamlessly swapping between docked and handheld modes. If the Switch 2 is connected to the network via ethernet when docked, swapping from docked to undocked requires the client to be restarted with the new IP provided.
  * A client can connect over a USB connection to the bottom port whilst in handheld mode. The top USB port or the dock's USB ports cannot be used for this method.
  * Only a USB 2.0 or higher connection is required.
  * Swapping connection mode does not require restarting the Switch 2! 
* Three client options:
    * A standalone OpenCV-powered client: multi-threaded, displays live FPS and bitrate stats in the window title, and includes a half-scale decode toggle.
      * *Toggled with* `H` *, half-scale decode reduces the client output to 960x540. Should improve playback performance if you are bottlenecked by decoding on the client side.*
    * A GUI to interface with the OpenCV-powered client: same full feature-set of the standalone client, with the addition of a user-friendly interface to minimise friction 
    * An ffplay-powered client: a simple, lightweight alternative.
* Stream module configuration:
    * `stream-conf.nro` allows for the audio stream to be toggled, the network stream to be toggled, and for the `stream-module` plugin to be stopped.
        * *plugins cannot be restarted without re-running kaiHEN from a fresh boot*
* Client and host monitoring:
    * Both client players report the received framerate, frame size, and bitrate
    * `cv2-client.py` also reports the network FPS (frames received per second), display FPS (frames decoded per second) to identify any bottlenecks in your setup.
    * `stream-conf.nro` can show useful information about the `stream-module` plugin and its connection: plugin status, capture status, capture & transmission framerates, client connection status, console IP, etc...
* Switching apps and visiting the home menu are handled safely
    * The capture method used is not able to function on the home screen or during a game's black boot screen, but deals with these transitions safely to prevent system panics
* Works in both docked and handheld mode
    * Stream continues without issue when swapping between docked and handheld, and minimal performance is lost when streaming in handheld mode.

## Known Issues

* Using this module disables the ability to take screenshots or record clips with the capture button
  * Future updates will let the plugin open and close the `caps2:s` service, letting the system's capture functionalities resume properly
* Poor stream performance
  * Nintendo removed the services that allowed for an encoded stream of the display to be captured, and the alternative method left has various quirks that result in significantly lowered performance. With current limitations of the exploit itself, as well as of kaiHEN, options like 720p, 60fps, or lower bandwidth are not possible. Expect 15-20fps when games are running, though 2D titles may still stream at 30fps.
* High bandwidth requirement
 * As mentioned above, this currently cannot be cut down. If you aren't able to get it running well enough over Wi-Fi, try streaming over USB as this should always have enough bandwidth (unavailable whilst docked). 

## Installation

### Switch 2 Installation
Make sure you are successfully able to boot into kaiHen before attempting installation.

1. Download `stream-conf.nro` and `stream-module.nso` from the latest release
2. Copy `stream-conf.nro` to `sdmc:/switch/SimpleStreamer`
3. Copy `stream-module.nso` to `sdmc:/kaihen/plugins`, creating the `plugins` folder if needed
    * If updating or replacing `stream-module.nso`, delete `sdmc:/kaihen/plugin-packages/stream-module.nsp`. You currently must reboot for changes to take effect

### Client Installation

Download `stream-client.zip` from the latest release, and extract `cv2-client.py`, `ffplay-client.py`, and `usb_transport.py` to the same folder.

## Usage

### Switch 2 Usage
 
With the correct files on the SD card, the stream should be available as soon as HEN launch is completed or the homebrew menu has been opened. Plugin status can be checked with `stream-conf.nro`

#### Streamer Config

`stream-module.nso` ships with all features enabled and default values for video & audio stream ports, but these can be edited at `sdmc:/switch/SimpleStreamer/capture.cfg`. 
If the file does not exist it will be created on first load, and any changes require a restart to apply.
If any issues are found the default values will be used and the problematic line will be logged in `sdmc:/switch/SimpleStreamer/logs/streamer.log`
The default `sdmc:/switch/SimpleStreamer/capture.cfg` is as seen below:

```
# SimpleStreamer boot configuration
# Booleans accept: true, false, 1, 0
# Ports accept: 1-65535 (video_port and audio_port must differ)
# Delete this file to regenerate defaults.
streaming_enabled = true
audio_enabled = true
usb_enabled = true
video_port = 8080
audio_port = 8081
```

### Client Usage
For GUI interface usage on Windows:
1. Open `client-streamer.exe` from the latest release tab
2. Enter your console's IP address (or tick `USB Streaming`)
3. Change the starting volume or toggle audio streaming entirely as required
4. Click start! The stream client will open and connect to your console, and the GUI will switch to its controls.
  * Change volume with the slider, by scrolling or by click & drag
  * Mute/unmute the stream with a button
  * Toggle half-scale decoding with a button
5. When you're done streaming, or want to change connection settings, use the provided button to cleanly close the stream. Alternatively, press `Esc` or `q` whilst focused on the stream window to do the same.

Users on all operating systems can also access the GUI through `client-GUI.py` bundled in `stream-client.zip` each release. [CustomTKinter](https://github.com/TomSchimansky/CustomTkinter) is also required for the GUI.

For direct `cv2-client.py` usage: 
1. Ensure `opencv-python` is installed
2. Run `python cv2_client.py <SWITCH2-IP>` or `python cv2_client --usb`
    * Press `h` to enable half-scale decoding. Useful if you are struggling to decode fast enough (Disp < Net in the stream stats)
    * Press `Up/Down Arrow` to increase and decrease stream volume in steps of 5% 
    * Press `m` to mute/unmute the stream
3. Press `Esc` or `q` to quit the stream.


For `ffplay-client.py`: 
1. Ensure `ffmpeg` is installed
2. Run `python ffplay-client.py <SWITCH2-IP>` or `python ffplay-client.py --usb`
    * Press `Up/Down Arrow` to increase and decrease stream volume in steps of 5%. Make sure you've clicked on the terminal first.
    * Press `m` to mute/unmute the stream. Make sure you've clicked on the terminal first.
3. Press `Esc` or `q` to quit the stream.

For USB streaming:
1. Connect the Switch 2's bottom USB-C port to your computer
  * First time setup requires using [zadig](https://zadig.akeo.ie/) to install the generic driver, see `First-time USB Setup` below for more info.
2. Both clients and the streamer keep the same usage steps; just replace `<SWITCH2-IP>` in the python command with `--usb`

## First-time USB Setup
Hardware limitations mean that only the bottom USB-C port can be used, as the top port is host-only.
As a result, USB streaming is not supported whilst docked.

1. Download and open [zadig](https://zadig.akeo.ie/)
2. Click Options, and ensure "List All Devices" is ticked
3. Find "Switch2 Streamer" in the list and select it. The USB ID should be 1209:BEEB
4. Click "Install Driver" and wait for zadig to announce completion. From there, you're ready to stream.

## Building
Only tested on Windows.
Ensure you have installed [devkitPro](https://devkitpro.org/) and [ounce-devkit](https://github.com/ouncebrew/ounce-devkit) properly.

### MSYS2/Bash
Run `make` in `stream-conf/` and `stream-module/` to build the respective `.nro` and `.nso`

### Powershell
Run `.\build.ps1` in `stream-conf/` and `stream-module/` to build the respective `.nro` and `.nso`

## Untested Environments
The following are some environments I have not tested the project in, and would love feedback on! Feel free to reach out to me in the r/switch2hacks discord and let me know how things work out, especially if part of your setup matches anything below.

* ~~USB 3.0 (or higher) connection~~
  * USB 3.0 connection has now been tested! The Switch 2 successfully negotiates a 5 Gbps SuperSpeed connection and can be used to minimise the risk of bandwidth bottlenecks.
* Weaker wireless connection, or ethernet
  * My current setup has the Switch 2 on an otherwise uncontested 80Mhz 5Ghz Wi-Fi 6 connection, and my client connected to the same network over 2.5Gbe. Data from users running both the Switch 2 and client over ethernet, both over wireless, or either with weaker wireless connections will help build a more representative idea of bandwidth requirements.
* Different client OS
  * The client device I was able to use for testing is running Windows 11. I don't have any devices running a different operating system, so I can't 100% confirm compatibility on MacOS for example.
* Weaker clients
  * The client device I was able to use for testing has a 7800X3D. I'm aware this is not an average-strength CPU, so more users providing data will help build a more representative idea of performance. 
* Alternate entrypoints
  * Civilization 7, etc.

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
  * [CustomTKinter](https://github.com/TomSchimansky/CustomTkinter)