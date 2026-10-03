# trusdx-mac-audio-bridge

Use a (tr)uSDX with WSJT-X, JS8Call or fldigi on macOS over **one USB cable**:
no sound card and no audio leads.

The truSDX firmware (2.00t+) can stream audio inside its USB serial link
([CAT extension](https://dl2man.de/5-trusdx-details/)). This project turns that
stream into a normal macOS audio device called **truSDX**, and puts the radio's
CAT control on a virtual serial port so your app can use both at the same time.

```
WSJT-X ──audio──▶ "truSDX" device ┐ (Core Audio plug-in in coreaudiod)
                                  │ cross-wired with a hidden twin device
                  TruSDXBridge ◀──┘
                     │   ▲
                     ▼   │ USB serial 115200: CAT + 8-bit audio
                    truSDX radio
WSJT-X ──CAT────▶ /tmp/trusdx-cat (pseudo-terminal owned by TruSDXBridge)
```

## Install

Download `truSDX-Bridge-<version>.pkg` from the Releases page and open it.
The package isn't signed yet, so macOS blocks it the first time: dismiss the
warning, then click **Open Anyway** in System Settings → Privacy & Security.
Installing restarts Core Audio, so other audio stops for a moment.

To build from source instead (Xcode Command Line Tools + `brew install cmake`):

```sh
scripts/install.sh    # builds, installs the driver (sudo, only if changed), launches the app
```

Uninstall with `/Applications/TruSDXBridge.app/Contents/Resources/uninstall.sh`.

**truSDX Bridge** lives in the menu bar as an antenna icon:

| Icon | Meaning |
|---|---|
| antenna | running, radio and audio connected |
| orange antenna | running, waiting for the radio's USB port or the audio driver |
| red antenna | transmitting |
| antenna with slash | stopped |

The menu shows the radio's frequency and has Start/Stop Bridge (⌘S),
Radio Speaker On, Open at Login, Show Log (`~/Library/Logs/trusdx-bridge.log`)
and About... (application information, copyright and licensing).
Stopping the bridge releases the serial port, so WSJT-X or a firmware tool can
use the radio directly again.

To run it in a terminal instead:

```sh
~/Applications/TruSDXBridge.app/Contents/MacOS/TruSDXBridge --headless -v
```

On first transmit, macOS asks for microphone access. Allow it: this is how the
bridge reads transmit audio from the truSDX device.

## WSJT-X settings

| Setting | Value |
|---|---|
| Radio → Rig | DL2MAN (tr)uSDX (or Kenwood TS-480) |
| Radio → Serial port | `/tmp/trusdx-cat` (type it in) |
| Radio → Baud rate | 115200 |
| Radio → Force Control Lines | **off** (a virtual port has no RTS/DTR; Hamlib fails to open it otherwise) |
| Radio → PTT method | **CAT** |
| Radio → Mode | None or USB |
| Audio → Input / Output | truSDX, Mono |

Start with WSJT-X's **Pwr** slider low and raise it until the signal is clean.
Check your signal with a second receiver: the radio's 8-bit TX path distorts
easily if driven too hard.

## Bridge options

```
--headless         run in the terminal instead of the menu bar
--port PATH        radio serial port (default: first /dev/cu.wchusbserial*)
--cat PATH         CAT link path (default /tmp/trusdx-cat)
--speaker          keep the radio's speaker on while streaming (UA1 instead of UA2)
--tx-gain X        scale transmit audio (default 1.0)
--tx-timeout SEC   force RX after this long keyed (default 180)
-v, --verbose      log CAT traffic and stats every 10 s
```

## How it works

- **Receive:** the radio sends `US` followed by unsigned 8-bit samples at about
  7.8 kHz. It pauses the stream with `;` whenever it answers a CAT command.
  The bridge removes the DC offset and resamples to 48 kHz. A slow control loop
  keeps about 80 ms of audio buffered, which follows the radio's own clock.
- **Transmit:** on `TX;`/`TX0;`/`TX1;` from the app, the bridge sends `TX0;US`.
  It then streams your audio at 11520 Hz, which is the full 115200-baud link
  rate. A `;` byte would end the stream, so sample value 0x3B is sent as 0x3A.
  CAT polls from the app during transmit are answered from cached replies.
  `RX;` sends `;RX;`, the bridge returns to receive, and streaming restarts.
- **The audio device:** a [libASPL](https://github.com/gavv/libASPL) plug-in
  publishes `truSDX` and a hidden `truSDX (bridge)`. Output on one device
  appears as input on the other. Both run on the same clock, so no audio data
  has to leave coreaudiod.
- **Safety:** the bridge unkeys (`;RX;`) at startup, on exit and on disconnect,
  and whenever a transmission exceeds `--tx-timeout`. The truSDX device can
  never become the system default, so alerts can't go out over the air.

## License

Copyright © 2026 Jan Szumiec. Licensed under the [MIT License](LICENSE).
The audio driver uses libASPL, copyright Victor Gaydov and contributors,
also licensed under the MIT License. Both licenses are included in the app
and linked from the About dialog.

## Development

```sh
cmake -S . -B build && cmake --build build -j && ctest --test-dir build
scripts/package.sh 0.3.0    # universal unsigned installer in dist/
```

CI (`.github/workflows/ci.yml`) builds, tests and packages every push and pull
request; the `.pkg` is attached to the run as an artifact. To publish a release:

```sh
git tag v0.3.0 && git push origin v0.3.0
```
