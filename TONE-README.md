# FM1 time-signal station
Status: host checks pass. Firmware builds with JieLi's Linux toolchain and
passes all upstream image checks. The tone build (instrument UI retained) was
flashed and worked on 2026-10-23; the station appliance build below was
flashed the same day. Frequency and on/off timing not yet measured. Not built with
Windows 2.5.3.

Based on m-vave/fm1_open commit 4c7496daac04a95ff10d1afdcc8f9ead2dbabc41.

## Behavior
Automatic 1 kHz sine, one second on, one second silent, repeating on both channels.
Peak approximately -18 dBFS before existing master gain. Incoming synth, USB and
Bluetooth audio are excluded from output. USB recording receives the test signal.
MASTER sets the output level. Flash operations may silence output and restart
the tone cycle.

## Measured timing (2026-10-23, one FM1, room temperature)
tools/measure_usb_tone.py captures the USB recording stream (asynchronous, so
the device clock sets the frame count) against the NTP-synchronized host:
- 2 s cycle: 2000.0154 ms, i.e. the audio clock is 7.7 ppm slow (about
  0.66 s/day); actual frame rate about 44117.66 Hz, not 44118.
- Tone on: 1000.008 ms; tone frequency 999.995 Hz.
- USB recording is resampled from the ~44118 Hz board rate to 44100 Hz;
  timing is preserved, the waveform carries small resampling artifacts.
Precision is limited by the host clock (about 1 ppm). Crystal temperature
drift remains, so the station clock needs a calibrated rate and periodic
computer synchronization.

## Station clock
app/clock.c keeps UTC time of day by counting audio frames at a calibrated
rate (default 44117.66 Hz), so seconds start on frame boundaries; the audio
ISR uses no division or 64-bit arithmetic. Haida Standard Time is
UTC-08:48 (default offset).

## Broadcast schedule (app/audio_output.c)
WWV-style, in local minutes; seconds match UTC because the offset is whole
minutes. Levels are before MASTER.
- Minute marker: 1000 Hz, 800 ms at :00, then 200 ms silence; at local
  minute 0 the hour marker is 1500 Hz. About -12 dBFS.
- Ticks: 1000 Hz, 5 ms (five cycles) at every other second except :29 and
  :59. About -12 dBFS.
- Protected zones: silence from 10 ms before to 30 ms after every tick or
  marker.
- Standard tones, seconds 1-44: 600 Hz odd minutes, 500 Hz even minutes,
  440 Hz minute 2; none in minutes 0, 3, 29, 30, 43-51 and 59. About
  -18 dBFS. tools/speech.py preview renders the same schedule.
PLAY lights while a standard tone or marker sounds.
Time starts unset at 00:00:00 UTC on power-on (no battery-backed clock);
offset and rate are not saved yet. Set them from an NTP-synchronized host:
    python3 -I tools/station.py sync --offset -8:48 --rate 44117.66
    python3 -I tools/station.py query
Protocol (USB MIDI SysEx, F0 7D 'T' 'S' 01 ...) is documented in app/transfer.c.
USB/MIDI latency is not yet calibrated (sync --latency-ms).

Panel: EDIT opens SET TIME with the current time, frozen. KNOB 1-3 turn
hours, minutes and seconds (each wraps without carrying); KNOB 4 right
selects UTC input, left local input (the entry converts, same instant).
SAVE accepts: the clock is set to the entered time, at .000, at that
moment, so dial a few seconds ahead and press on the beat. HOME cancels. EDIT stays lit
while the clock is unset.

## Speech clips
speech/clips.tsv lists the announcement vocabulary (English, whole numbers
0-59, station ID for VA7BFR). tools/speech.py generates placeholder clips
with Piper text-to-speech (en_US-john-medium, public-domain training data),
records your own voice, processes clips to 8 kHz and renders a broadcast
preview of the end of any minute. Generated audio stays under build/.

## Station appliance
The instrument UI, projects, presets, DX7 factory voices, voice library,
sample bank, MIDI routing and workbench file transfers are removed. The SDK
engine still runs and calls the app entry points; they are inert:
- app/screen.c: status display (tone state, uptime, MASTER level, audio
  overruns), MASTER knob, PLAY LED lit during the tone.
- app/transfer.c: application SysEx hook, ignored for now; computer time
  setting will arrive here. Firmware updates are handled by the SDK first.
- app/midi_router.c: sends no MIDI. app/project.c: no storage; turns
  Bluetooth off; keeps BLE and engine services the build checks require
  linked (retained table, not called).
User Flash is not read or written. The workbench "Projects & sounds" tab no
longer has a device protocol to talk to.

App image: 372,800 bytes; 81,121 bytes of headroom (the tone build had 245).
The binary SDK engine is 375 KB of any image and cannot be removed.

Timing uses the board driver's declared 44118 Hz. Actual frequency and duration
depend on the hardware clock and require measurement. The public nominal rate
44100 differs from the board definition. No wall clock or synchronization is used.
The sine is hard-gated at each boundary; no additional envelope is applied.
A quarter-wave table (65 entries) keeps SRAM use small: upstream leaves only
345 bytes of app image headroom, and a full 256-entry table overflows it.
The appliance build no longer needs that economy, but it keeps the table.

## Build on Windows x64
Follow upstream README.md to install JieLi Windows toolchain 2.5.3.
In PowerShell from this folder:
    ./sdk/Build.ps1 -FirmwareOnly -Toolchain 'C:\JL\pi32'

Expected image:
    build/dx7/generated/FM1_OFFICIAL_OPEN.fwsc

Keep the manifest and link checks enabled. This modified source has passed
them with the Linux toolchain only. Back up projects, samples and voices before flashing.
Use the official FM1 workbench update process described in upstream README.md.
The update/UI identity remains upstream's for this initial local test.
Do not mistake sdk/firmware/FM-1_015.fwsc for our image: it is the stock firmware.

## Build on Linux x64
JieLi publishes a Linux toolchain with the same clang 4.0.1 release (its
compiler headers match sdk/manifest.json). tools/linux-toolchain.json pins
the exact archive and program hashes. Extract it outside the source tree:
    curl -LO https://jl-update.oss-cn-shenzhen.aliyuncs.com/jieli-linux-toolchains-20260730.1.tar.xz
    tar -xJf jieli-linux-toolchains-20260730.1.tar.xz
    python3 -I tools/build_linux.py --toolchain jieli-linux-toolchains-20260730.1

tools/build_linux.py ports Build.ps1 -FirmwareOnly and sdk/Package.cs. It
keeps every SDK, ABI, symbol, SRAM, link map and section check; only the
toolchain program hashes come from tools/linux-toolchain.json instead of
sdk/manifest.json. Output is the same build/dx7/generated/FM1_OFFICIAL_OPEN.fwsc.
Code generation equivalence with the Windows 2.5.3 binaries is not established.

To flash, the firmware must be embedded in an update page (the workbench has
no firmware file picker). Add --web with an extracted WASI SDK 27 for Linux
(SHA-256 b7d4d944c88503e4f21d84af07ac293e3440b1b6210bfd7fe78e0afd92c23bc2):
    python3 -I tools/build_linux.py --toolchain <jieli> --web \
      --wasi-sdk wasi-sdk-27.0-x86_64-linux --source-url <https source URL>
Output: build/official-web/FM1-Official-Open-Source.html, with stock V15
recovery embedded. Serve it from localhost (Web MIDI needs a secure context):
    python3 -m http.server -b 127.0.0.1 -d build/official-web 8000
Do not distribute that page until --source-url points at this modified source.

The page's "Play & edit" tab runs web/engine/Simulator.c, a browser build of
the same app sources: the station display, controls, MASTER and the tone
itself (the SDK engine is absent and treated as silent). sdk/Simulator.js
and sdk/Update.html.in are SDK-pinned, so their instrument panels remain
visible but inert.

## Host verification
    gcc -std=c11 -Wall -Wextra -Werror -DFM1_HOST_TEST -Isdk/include -Iapp app/audio_output.c tests/tone_host.c -o tone-test
    ./tone-test

Checks the actual C renderer, including crossing frequency, silence boundaries
across blocks, amplitude, matching channels, zero master and input isolation.
These checks do not validate PI32 compilation, timing on hardware or flashing.

Modified: app/audio_output.c, app/audio_output.h, app/screen.c, app/project.c,
app/midi_router.c, app/transfer.c, app/fm_patch.h, web/engine/Simulator.c,
tools/Build-Web.ps1
Removed: other app instrument sources, web/engine instrument emulation,
web/synth, third_party/synth
Added: tests/tone_host.c, TONE-README.md, tools/build_linux.py,
tools/linux-toolchain.json
Retain upstream licensing and SDK notices.
