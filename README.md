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
-v, --verbose      log virtual CAT reads/writes, radio CAT and stats every 10 s
```

The menu-bar **Verbose Logging** checkbox enables the same diagnostics immediately,
without restarting the bridge, and remembers the setting for future launches.
Use **Show Log** to open `~/Library/Logs/trusdx-bridge.log`. Terminal/headless runs
write logs to stderr.

Virtual CAT entries show raw read chunks and bytes actually written to the client,
including local `ID020;` and cached TX status replies. Byte counts, escaped control characters,
and failed/partial writes make missing or malformed replies visible. For example:

```text
20:00:00.123 virtual CAT <- client (5 bytes) "ID;\r\n"
20:00:00.124 virtual CAT -> client (6/6 bytes) "ID020;"
```

One read chunk can contain several commands or only part of a command. Radio CAT
entries describe physical-radio commands/replies separately; internal radio replies
suppressed from the virtual port do not appear as virtual CAT output.

## How it works

- **Receive:** the radio sends `US` followed by unsigned 8-bit samples at about
  7.8 kHz. It pauses the stream with `;` whenever it answers a CAT command.
  The bridge removes the DC offset and resamples to 48 kHz. A slow control loop
  keeps about 80 ms of audio buffered, which follows the radio's own clock.
  After each TX-to-RX transition (including a transmit timeout), the bridge
  immediately resets serial audio: unkey, wait 50 ms, `UA0;`, wait 200 ms,
  selected `UA1;`/`UA2;`, wait 50 ms, then `RX;`. It waits up to another
  500 ms for new samples. The same sequence previously recovered a measured
  stall on firmware 2.00x; starting it immediately is an experimental workaround.
  The serial writer advances these delays without blocking cached FA/MD/IF
  replies, with IF reporting RX. Setters and another TX stay ordered behind
  the reset; internal reset acknowledgements and errors stay out of client CAT.
  If samples remain absent, the watchdog retries after at least five seconds
  between attempts, two seconds without samples and one second in RX.
  Repeated RX commands while already receiving do not trigger a reset.
  This resets the radio's serial stream; it does not restart CoreAudio.
- **Transmit:** uses the audio encoding and blocks from the
  [vendor reference bridge](https://dl2man.de/wp-content/uploads/2022/01/wp.php/trusdx-audio.zip),
  but keeps routine CAT traffic off the serial link while keyed. Interrupting
  audio to forward polling queries causes RF gaps and failed FT8 decoding.
  `ID` is always answered locally as `ID020;`. During TX, `FA;`, `MD;` and
  `IF;` queries receive immediate replies using the last confirmed radio state.
  The IF reply uses the cached frequency/mode and the bridge's commanded TX flag.
  Startup reads FA, MD and IF sequentially to prime this cache; RX replies refresh
  it. Front-panel changes during TX are reflected only after a later RX readback.
  Missing state, unsupported queries and setters receive `?;`; they are not held
  until RX or sent into the audio stream. The experimental `O;` busy response
  caused WSJT-X's bundled Hamlib to report an IF protocol error and abort TX,
  so it is not used. Repeated TX commands do not
  restart audio. In RX, commands are forwarded unchanged and in order,
  without calling `tcdrain`: on macOS that call could stall the writer for
  seconds even after a radio reply arrived, causing the next query to time out.
  To unkey, the bridge stops capture, allows 60 ms for the final audio block,
  writes `;` separately, waits 10 ms, then writes `RX;`. A 512-byte block takes
  44.4 ms at 115200 baud. This avoids discarding serial output and the measured
  3.3-second macOS drain delay. A status query queued behind unkey waits about
  70 ms for this handoff, then uses the recovery cache described above.
  It retains that same unkey path for
  the transmit timeout and shutdown. TX audio is unsigned 8-bit samples at
  11520 Hz, in 512-sample blocks, without a host `US` marker. Capture is reset
  on TX and the first block discarded. Signed 16-bit PCM is converted with
  `128 + sample // 256`; sample value 0x3B is replaced with 0x3A. No silence
  filler is generated. Startup waits three seconds, selects
  USB (`MD2;`),
  and enables `UA1;` or `UA2;`.
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
