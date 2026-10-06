# trusdx-mac-audio-bridge

Use a (tr)uSDX with WSJT-X, JS8Call or fldigi on macOS over **one USB cable**:
no sound card and no audio leads.

The radio's [CAT audio extension](https://dl2man.de/5-trusdx-details/)
streams audio inside its USB serial link. The
[vendor manual](https://dl2man.de/4-trusdx-manual/) specifies firmware **2.00u or
newer** for USB audio streaming; this bridge's hardware tests used **2.00x**.
This project turns that stream into a normal macOS audio device called **truSDX**,
and puts CAT control on a virtual serial port so your app can use both at once.

```
Audio: WSJT-X ⇄ "truSDX" device ⇄ hidden bridge device ⇄ TruSDXBridge
CAT:   WSJT-X ⇄ /tmp/trusdx-cat                       ⇄ TruSDXBridge
USB:   TruSDXBridge ⇄ truSDX radio (CAT + 8-bit audio, 115200 baud)
```

## Install

Requires **macOS 13 or newer**. Download `truSDX-Bridge-<version>.pkg` from
[Releases](https://github.com/jasiek/trusdx-mac-audio-bridge/releases) and open it.
The installer supports Apple silicon and Intel Macs. It is unsigned and not
notarized; if macOS blocks it, dismiss the warning, then use **Open Anyway** in
System Settings → Privacy & Security.

The package installs the app in `/Applications/TruSDXBridge.app` and the audio
driver in `/Library/Audio/Plug-Ins/HAL/truSDX.driver`. Installation stops any
running bridge, restarts Core Audio (briefly interrupting other audio), and
attempts to launch the menu bar app for the logged-in desktop user.

To build from source instead (Xcode Command Line Tools + `brew install cmake`):

```sh
scripts/install.sh    # builds, installs the driver (sudo, only if changed), launches the app
```

The source installer puts the app in `~/Applications/TruSDXBridge.app`; the
driver location is the same. It requires administrator access when replacing
the driver.

Uninstall a package installation with
`/Applications/TruSDXBridge.app/Contents/Resources/uninstall.sh`, or a source
installation with `~/Applications/TruSDXBridge.app/Contents/Resources/uninstall.sh`.
The uninstaller removes both app locations and the driver, requires administrator
access, and restarts Core Audio.

**truSDX Bridge** lives in the menu bar as an antenna icon:

| Icon | Meaning |
|---|---|
| antenna | running, radio and audio connected |
| orange antenna | radio/audio connection is not ready, or the bridge detects a radio audio-stream problem |
| red antenna | bridge has commanded transmit |
| antenna with slash | stopped |

The menu shows the last reported radio frequency, audio connection status and
virtual CAT path. It has Start/Stop Bridge (⌘S), Radio Speaker On, Open at Login,
Verbose Logging, Show Log (⌘L), About... (application information, copyright and
licensing), and Quit truSDX Bridge (⌘Q).
Changing Radio Speaker On and toggling Verbose Logging take effect without
restarting the bridge, so a connected CAT client keeps its port. A speaker change
is applied with a serial-audio reset (see below); during TX it waits for the reset
that follows unkey. The app remembers the Start/Stop, speaker
and logging choices. Open at Login controls whether macOS launches the app.
Stopping the bridge releases the serial port, so WSJT-X or a firmware tool can
use the radio directly again.

To run a package installation in a terminal, first quit the menu bar app:

```sh
/Applications/TruSDXBridge.app/Contents/MacOS/TruSDXBridge --headless -v
```

For a source installation, use the same command under `~/Applications`.
Run only one bridge instance at a time. Headless mode uses command-line options,
not the menu bar app's saved speaker/logging preferences.

If macOS asks for microphone access, allow it so the bridge can capture transmit
audio from the virtual device. Audio I/O starts when the bridge connects to the
device, so a permission prompt need not wait until the first transmission.

## WSJT-X settings

| Setting | Value |
|---|---|
| Radio → Rig | Kenwood TS-480 (used in the compatibility tests) |
| Radio → Serial port | `/tmp/trusdx-cat` (type it in) |
| Radio → Baud rate | 115200 |
| Radio → Force Control Lines | **off** (a virtual port has no RTS/DTR; Hamlib fails to open it otherwise) |
| Radio → PTT method | **CAT** |
| Radio → Mode | None or USB |
| Audio → Input / Output | truSDX, Mono |

Point WSJT-X at the **virtual** CAT path, not the radio's physical USB serial
port. The bridge owns that physical port. The radio implements a subset of
TS-480 commands; unsupported meter/keyer commands such as `RM`/`KS` can still
produce Hamlib warnings. The bridge does not emulate all TS-480 features.

Start with WSJT-X's **Pwr** slider low and raise it until the signal is clean.
Check your signal with a second receiver: the radio's 8-bit TX path distorts
easily if driven too hard.

## Bridge options

```
--headless         run in the terminal instead of the menu bar
--port PATH        physical radio serial port (default: auto-detect; see below)
--cat PATH         CAT link path (default /tmp/trusdx-cat)
--speaker          keep the radio's speaker on while streaming (UA1 instead of UA2)
--tx-gain X        scale transmit audio (default 1.0)
--tx-timeout SEC   force RX after this long keyed (default 180)
--rx-rate HZ       nominal radio receive sample rate (default 7812.5)
-v, --verbose      log virtual CAT reads/writes, radio CAT and periodic stats
```

`--tx-gain` and `--rx-rate` must be numbers greater than zero, and `--tx-timeout`
whole seconds from 1 to 86400. An invalid value prints an error and the usage
text, and the bridge exits with status 2.

Autodetection selects the first `/dev/cu.wchusbserial*` match, falling back to
the first `/dev/cu.usbserial*` match. It does not probe devices to identify the
radio. Use `--port PATH` for another device name or to select a specific radio.
The physical port uses 115200 baud, 8N1, no hardware flow control, DTR high and
RTS low. The CAT pseudo-terminal does not expose those hardware control lines.

The menu-bar **Verbose Logging** checkbox enables the same diagnostics immediately,
without restarting the bridge, and remembers the setting for future launches.
Use **Show Log** to open `~/Library/Logs/trusdx-bridge.log`. Terminal/headless runs
write logs to stderr. CAT events are logged as they occur; statistics are logged
approximately every ten seconds while verbose logging is enabled.

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

### Startup and audio routing

Each physical-radio connection waits about three seconds for boot, sends
`;RX;MD2;` to unkey and select USB, and enables streaming with `UA1;` (speaker
on) or `UA2;` (speaker off). It waits up to 1.5 seconds for audio, then queries
`FA;`, `MD;` and `IF;` sequentially to initialize cached state, allowing up to
500 ms for each reply. Startup replies are withheld from the CAT client.
Reconnecting repeats this initialization, including selecting USB.

The [libASPL](https://github.com/gavv/libASPL) Core Audio driver publishes the
visible **truSDX** device and a hidden **truSDX (bridge)** device. Both are mono,
48 kHz devices using a shared clock. The driver routes output from either device
to the other's input inside `coreaudiod`; the bridge is a separate Core Audio
client that converts those samples to and from serial audio.

On receive, the radio sends `US` followed by unsigned 8-bit samples at a nominal
7812.5 Hz. A semicolon ends an audio segment so CAT replies can share the link;
`US` starts the next segment. The bridge removes DC offset and resamples to
48 kHz, adjusting the conversion rate to follow the radio clock while targeting
about 80 ms of buffered audio.

On transmit, the bridge captures 48 kHz float audio and resamples to 11520 Hz,
sending unsigned 8-bit samples in 512-byte blocks. Its encoding and block
handling follow the
[vendor reference bridge](https://dl2man.de/wp-content/uploads/2022/01/wp.php/trusdx-audio.zip):
reset the capture buffer and resampler on TX, discard the first resampled block,
omit the host `US` marker, and generate no silence filler. Float samples are
clamped and converted to signed 16-bit values, then encoded as
`128 + sample // 256` with floor division. Sample value `0x3B` is replaced with
`0x3A` because a semicolon would terminate the stream.

### CAT handling

CAT commands are semicolon-terminated; incoming carriage returns and newlines
are ignored. `ID` is handled locally as `ID020;`, even without a radio connection,
so a successful ID query alone does not prove the radio is connected. Other
commands enter an ordered writer queue when the physical port is open.

| State when processed | Command | Behavior |
|---|---|---|
| Normal RX | Commands other than local `ID` | Forward to the radio without `tcdrain`; the reader delivers replies independently. |
| TX | `FA;`, `MD;`, `IF;` | Reply from the last confirmed radio state; `IF` uses cached frequency/mode and the bridge-commanded TX flag. Missing required state returns `?;`. |
| TX | Another TX command | Ignore it; do not restart audio. |
| TX | RX command | End TX using the handoff below, then start serial-audio recovery. |
| TX | Other queries or setters | Return `?;`; do not send them into the audio stream or defer them until RX. |
| Serial-audio recovery | `FA;`, `MD;`, `IF;` at the queue head | Reply from the cache, with `IF` reporting RX. |
| Serial-audio recovery | `RX;` at the queue head | Absorb the redundant request. |
| Serial-audio recovery | Other queued commands | Wait until recovery finishes. Later commands remain behind them. |

Normal RX replies refresh the cache. A well-formed `FA` or `MD` setter forwarded
in RX also updates it, because the radio applies these without replying; this
keeps TX replies correct when a client shifts frequency just before keying (for
example WSJT-X "Fake It" split). Front-panel changes during TX may remain
stale until a later RX readback. The displayed frequency also comes from this
cache; the bridge does not continuously poll it on its own.

Keeping routine CAT queries off the serial link during TX prevents the audio
interruptions measured when forwarding those queries. The experimental `O;`
busy response caused WSJT-X's bundled Hamlib to report an IF protocol error and
abort TX, so it is not used. In RX, removing `tcdrain` avoids a measured macOS
writer stall that held up the next query even after the radio had answered the
previous one.

### TX-to-RX handoff and recovery

To unkey an active transmission, the bridge stops accepting capture samples,
waits 60 ms for the last audio block, writes `;` separately, waits 10 ms, then
writes the client's RX command (or `RX;` for a timeout/shutdown). A 512-byte
block takes about 44.4 ms at 115200 baud with 8N1 framing. This avoids discarding
queued output or waiting in the driver's potentially multi-second `tcdrain`.
The 60/10 ms waits are requested delays; OS scheduling and I/O can make the
handoff take longer than 70 ms.

During a running session, an actual TX-to-RX transition, including a transmit
timeout, immediately schedules this serial-audio reset:

1. Wait 50 ms after unkey, then send `UA0;`.
2. Wait 200 ms, then send the configured `UA1;` or `UA2;`.
3. Wait 50 ms, then send `RX;`.
4. Wait up to another 500 ms for new sample bytes.

The writer advances recovery deadlines without sleeping through the entire
reset. Cached status queries at the queue head can be answered during recovery,
but a preceding setter, TX or other deferred command blocks later queries until
recovery finishes. Replies with a `UA` prefix and `?;` errors are hidden from the
client during recovery. Repeated `RX;` while already receiving does not itself
trigger a reset.

If this end-of-TX reset gets no samples, the bridge immediately retries once,
sending `RX;` before the same sequence and keeping replies hidden throughout.
If the retry also fails, recovery is left to the watchdog below. Watchdog and
speaker-change resets are not retried immediately.

If RX has previously produced samples and then stalls, the watchdog starts a
reset after more than two seconds without samples and one second in RX, and at
least five seconds after the previous reset started. The watchdog sends `RX;`
before the same reset sequence. It does not cover a connection that has never
produced any sample bytes.

This resets the radio's serial stream, not the Core Audio device registration.
The installed driver continues to publish the audio device when the bridge is
stopped. Separately, the bridge attempts to reconnect missing or unhealthy Core
Audio I/O; that mechanism does not resolve the startup hang described below.

### Unkeying and output routing

The bridge sends an unkey command during radio initialization, on orderly stop
or exit while the serial connection is usable, and when its TX timeout expires
(default 180 seconds). A failed or unplugged serial connection cannot deliver
that command, and the timeout cannot guarantee unkeying after a process or I/O
hang. The bridge attempts to reconnect a lost radio and unkeys during the next
initialization.

The driver marks both audio devices ineligible as the macOS default audio or
system-sound device. An application can still explicitly select the truSDX
output; while TX is active, audio sent there is eligible for transmission.

## Tested configuration and known limitations

The October 5, 2026 hardware tests used a truSDX with **user-reported firmware
2.00x** from the [vendor beta page](https://dl2man.de/wp-content/uploads/2022/01/wp.php/beta.html),
USB serial at 115200 8N1, and the native 48 kHz Core Audio driver on macOS.
Final runs enabled the radio speaker (`UA1`). The antenna/load and power
arrangement was confirmed by the operator; supply voltage and RF output power
were not measured.

Controlled FT8 transmissions used 14.074 MHz USB and `CQ HF2J KO02`, with
1005 Hz audio resampled to 48 kHz at a peak amplitude of 0.30. During each final
transmission, a test client sent 600 CAT queries. An independent Nooelec NESDR
SMArt v5 on its own receive antenna captured RF in Q direct-sampling mode,
AGC off, at 250 ksps centered on 13.960 MHz. Captures were USB-demodulated and
decoded with WSJT-X's `jt9` decoder.

Five bounded-handoff TX/RX cycles recovered receive audio without a watchdog
retry. In the final RX CAT before/after test, the old build stalled a query
for 2.85 seconds; the fixed build answered 20 consecutive queries in 5–21 ms each. The fixed run decoded the transmitted FT8 message and delivered
240,000 nonzero frames in a five-second receive capture. Detailed measurements
and the diagnosis are recorded in
[the reliability commit](https://github.com/jasiek/trusdx-mac-audio-bridge/commit/8d7357d9d6bcc38493a73926932e7fa668e5c010).

- These tests demonstrate controlled local RF, CAT and audio behavior; they do
  not establish sustained native WSJT-X GUI reliability or PSKReporter spotting.
  They do not establish compatibility with other firmware versions or validate
  full JS8Call/fldigi workflows.
- An intermittent Core Audio startup delay/hang remains unresolved. Tests saw
  stalls in `AudioDeviceStart`/`AudioDeviceCreateIOProcID`, including OSStatus
  `268435460`; one successful launch took about 44 seconds to connect. The
  serial-audio reset is not a fix for this problem.
- CAT support is limited to the radio's command subset and the local behavior
  above. Unsupported commands, stale cached front-panel state during TX, and
  queries behind deferred commands during recovery remain limitations.

## License

Copyright © 2026 Jan Szumiec. Licensed under the [MIT License](LICENSE).
The audio driver uses libASPL, copyright Victor Gaydov and contributors,
also licensed under the MIT License. Both licenses are included in the app
and linked from the About dialog.

## Development

```sh
cmake -S . -B build -DTRUSDX_VERSION=0.3.0
cmake --build build -j
ctest --test-dir build --output-on-failure
scripts/package.sh 0.3.0    # universal unsigned installer in dist/
```

CI (`.github/workflows/ci.yml`) builds, tests and packages pushes to `main`,
`v*` tags, pull requests and manual runs. Both test suites run without a physical
radio or Core Audio I/O: the session suite uses the real bridge with a fake PTY
radio and controlled capture. The package script verifies that both app and
driver contain arm64 and x86_64 binaries. The unsigned `.pkg` is attached to the
workflow run; tag builds also publish it as a GitHub Release after tests pass.

For a new release, update the CMake project version, commit and push the intended
changes, then tag that commit with a new version. For example (choose an unused
version; `v0.3.0` is already published):

```sh
VERSION=0.3.1
git tag -a "v$VERSION" -m "Release v$VERSION"
git push origin "v$VERSION"
```

The tag supplies the package and bundle version. For local builds, CMake caches
`TRUSDX_VERSION`; pass `-DTRUSDX_VERSION=<version>` when reusing a build directory
for a new version. Verify the release workflow and installer asset, and add
release notes covering changes, tested firmware and remaining limitations.
