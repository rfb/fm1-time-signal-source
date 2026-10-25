# FM1 time-signal source

An M-VAVE FM1 synthesizer repurposed as a WWV-style time-signal station that
feeds an AM/SSB transmitter, keeping Haida Standard Time (UTC−08:48) for
amateur station VA7BFR.

To give Haida Standard Time a voice on the air, starting in English and
built to carry Xaayda Kil announcements recorded with a Haida speaker.

Based on [m-vave/fm1_open](https://github.com/m-vave/fm1_open) commit
4c7496daac04a95ff10d1afdcc8f9ead2dbabc41. The instrument is stripped to a
station appliance; the binary SDK engine (375 KB of any image) remains.

## Broadcast (app/audio_output.c)
WWV-style, in local minutes; seconds match UTC because the offset is whole
minutes. Levels are before MASTER.
- Minute marker: 1000 Hz, 800 ms at :00, then 200 ms silence; at local
  minute 0 the hour marker is 1500 Hz. About -12 dBFS.
- Ticks: 1000 Hz, 5 ms (five cycles) every second except :00, :29 and
  :59. About -12 dBFS.
- Protected zones: silence from 10 ms before to 30 ms after every tick or
  marker. Speech pauses in them too, like WWV.
- Standard tones, seconds 1-44: 600 Hz odd minutes, 500 Hz even minutes,
  440 Hz minute 2; none in minutes 0, 3, 29, 30, 43-51 and 59. About
  -18 dBFS. A second that carries speech has no tone.
- "At the tone, H hour(s), M minute(s), Haida Standard Time." for the next
  minute marker, starting at :52.5 every minute.
- Station identification (VA7BFR) at :02.03 of minutes 00 and 30.

Nothing is announced while the clock is unset; ticks, markers and tones
still play. `tools/speech.py preview` renders the same schedule.

## Station clock (app/clock.c)
Keeps UTC time of day by counting audio frames at a calibrated rate
(default 44117.66 Hz), so seconds start on frame boundaries; the audio ISR
uses no division or 64-bit arithmetic. The offset defaults to Haida
Standard Time, UTC-08:48.

Time starts unset at 00:00:00 UTC on power-on (no battery-backed clock);
offset and rate are not saved yet. Set them from an NTP-synchronized host:
    python3 -I tools/station.py sync --offset -8:48 --rate 44117.66
    python3 -I tools/station.py query
The protocol (USB MIDI SysEx, F0 7D 'T' 'S' 01 ...) is documented in
app/transfer.c. USB/MIDI latency is not yet calibrated (sync --latency-ms).

### Measured timing (2026-10-23, one FM1, room temperature)
tools/measure_usb_tone.py captures the USB recording stream (asynchronous,
so the device clock sets the frame count) against the NTP-synchronized host:
- The audio clock is 7.7 ppm slow (about 0.66 s/day): actual frame rate
  about 44117.66 Hz, not the board's declared 44118.
- Tone frequency within 10 ppm at the calibrated rate.
- USB recording is resampled to 44100 Hz; timing is preserved, the waveform
  carries small resampling artifacts.
Precision is limited by the host clock (about 1 ppm). Crystal temperature
drift remains, so the clock needs periodic computer synchronization.

## Panel (app/screen.c)
The display shows local time, zone, UTC, sync state, MASTER and audio
health. MASTER sets the output level.
- EDIT opens SET TIME with the current time, frozen. KNOB 1-3 turn hours,
  minutes and seconds (each wraps without carrying); KNOB 4 right selects
  UTC input, left local input (the entry converts, same instant). SAVE sets
  the clock to the entered time, at .000, at that moment, so dial a few
  seconds ahead and press on the beat. HOME cancels.
- EDIT stays lit while the clock is unset.
- The first note key queues an extra station ID at the next :02.03 at least
  half a second away (press again to cancel). It stays lit, and the status
  line reads STATION ID QUEUED, until that ID has played. A queued ID waits
  while the clock is unset or no bank is loaded.

## Speech
Each language has a folder under speech/ (speech/en/ for English). Its
clips.tsv lists the announcement vocabulary (whole numbers 0-59, the words
of the announcement, the station ID): id, text, say (text-to-speech
pronunciation), gloss (English meaning, for other languages);
language.json names the language and credits its speakers.

### Clip collector (tools/collector.html)
A page for recording every clip of a language with a speaker. `build_linux.py
--web` puts it beside the flasher as build/official-web/collector.html:
    python3 -m http.server -b 127.0.0.1 -d build/official-web 8000
    # Chrome or Edge: http://127.0.0.1:8000/collector.html
Open the repository's speech/ folder. Pick a language, or create one ("New
language": a folder code such as hax for Xaayda Kil or hdn for Xaad Kil, the
language name, the speakers' credit and notes). A new language starts with
the station's clip ids and the English as each clip's gloss; type the words
as spoken while recording. Takes are written as 48 kHz 16-bit WAV to
speech/<code>/recordings/<id>.wav (a replaced take moves to
recordings/previous/). The page shows the speech length and an estimate of
the bank size, warns about clipping, quiet takes and speech cut at either
end, and plays a take as the station will (trimmed, 8 kHz). The microphone
choice is remembered; a Bluetooth headset's microphone switches its
playback to narrowband call audio. Recordings are git-ignored: publish a
speaker's voice only with their permission.

### Speech bank (tools/speech.py, app/speech.c)
    python3 -I tools/speech.py process     # trim, level, 8 kHz
    python3 -I tools/speech.py bank        # build/speech/en/bank.bin
    python3 -I tools/station.py upload build/speech/en/bank.bin
`--lang <code>` (default en) selects the language. speech.py can also
generate placeholder clips with Piper text-to-speech (en_US-john-medium,
public-domain training data) and render a broadcast preview of the end of
any minute; generated audio stays under build/speech/<code>/.

The bank (format in app/speech.h) holds the clips as 8 kHz 4-bit IMA ADPCM
(4000 bytes per second) in the former sample region of user Flash (offset
69632, up to 466944 bytes; the English bank is 214 KB). Clips are found by
name. Upload erases as it goes and commits only after a CRC check of the
stored bank, so an interrupted or damaged upload never plays; the station is
briefly silent while Flash is written, and the audio-clocked station clock
pauses (about 3 s for 200 KB); `upload` re-syncs it afterwards when it was
set. The audio ISR decodes from the mapped partition and resamples to the
board rate. The firmware plays one bank, in the English announcement order.

## Station appliance
The instrument UI, projects, presets, DX7 factory voices, voice library,
sample bank, MIDI routing and workbench file transfers are removed. The SDK
engine still runs and calls the app entry points:
- app/transfer.c: the station's SysEx control. Firmware updates are handled
  by the SDK first.
- app/midi_router.c: sends no MIDI. app/project.c: no storage; turns
  Bluetooth off; keeps BLE and engine services the build checks require
  linked (retained table, not called).
Incoming synth, USB and Bluetooth audio are excluded from output; USB
recording receives the station signal. The workbench "Projects & sounds"
tab no longer has a device protocol to talk to.

App image: 379,996 bytes, about 74 KB of headroom. A quarter-wave sine table
(65 entries) is kept from the tone build, when upstream left only 345 bytes.

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
sdk/manifest.json. Output: build/dx7/generated/FM1_OFFICIAL_OPEN.fwsc.
Code generation equivalence with the Windows 2.5.3 binaries is not
established. Do not mistake sdk/firmware/FM-1_015.fwsc for our image: it is
the stock firmware.

To flash, the firmware must be embedded in an update page (the workbench has
no firmware file picker). Add --web with an extracted WASI SDK 27 for Linux
(SHA-256 b7d4d944c88503e4f21d84af07ac293e3440b1b6210bfd7fe78e0afd92c23bc2):
    python3 -I tools/build_linux.py --toolchain <jieli> --web \
      --wasi-sdk wasi-sdk-27.0-x86_64-linux --source-url <https source URL>
Output: build/official-web/FM1-Official-Open-Source.html (with stock V15
recovery embedded) and the clip collector. Without --web, previous pages are
removed. Serve them from localhost (Web MIDI needs a secure context):
    python3 -m http.server -b 127.0.0.1 -d build/official-web 8000
Do not distribute that page until --source-url points at this modified
source. After flashing, check with `station.py query`: the clock must be
synced again. Back up device data before flashing.

The page's "Play & edit" tab runs web/engine/Simulator.c, a browser build of
the same app sources: the station display, controls, MASTER and the signal
(the SDK engine is absent and treated as silent). sdk/Simulator.js and
sdk/Update.html.in are SDK-pinned, so their instrument panels remain visible
but inert.

## Build on Windows x64
Not yet tried with this source. Install JieLi Windows toolchain 2.5.3 as the
upstream README describes, then in PowerShell from this folder:
    ./sdk/Build.ps1 -FirmwareOnly -Toolchain 'C:\JL\pi32'
Keep the manifest and link checks enabled.

## Host verification
    gcc -std=c11 -Wall -Wextra -Werror -DFM1_HOST_TEST -Isdk/include -Iapp \
      app/audio_output.c app/clock.c app/speech.c tests/tone_host.c -o tone-test
    gcc -std=c11 -Wall -Wextra -Werror -DFM1_HOST_TEST -Isdk/include -Iapp \
      app/clock.c app/speech.c tests/speech_host.c -o speech-test
    ./tone-test && ./speech-test

tone_host checks the actual C renderer: markers, ticks, protected zones, the
tone schedule, levels, muted master and input isolation. speech_host checks
the scheduler, the queued station ID and bank upload against the built
English bank (build/speech/en). These do not validate PI32 compilation,
timing on hardware or flashing.

## License
APP: [GPLv3 with the FM1 SDK linking exception](LICENSE.md). The device engine is supplied as a binary SDK with separate terms. Retain the applicable [SDK terms, third-party licenses and contributor notices](THIRD_PARTY.md) when distributing builds.
