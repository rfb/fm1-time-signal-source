# FM1 1 kHz tone test
Status: host checks pass. Firmware builds with JieLi's Linux toolchain and
passes all upstream image checks. Flashed to an FM1 and working (2026-10-23).
Frequency and on/off timing not yet measured. Not built with Windows 2.5.3.

Based on m-vave/fm1_open commit 4c7496daac04a95ff10d1afdcc8f9ead2dbabc41.

## Behavior
Automatic 1 kHz sine, one second on, one second silent, repeating on both channels.
Peak approximately -18 dBFS before existing master gain. Incoming synth, USB and
Bluetooth audio are excluded from output. USB recording receives the test signal.
The stock instrument UI and engine still run. Master gain follows existing panel
behavior; this is a small output replacement, not a stripped-down clock firmware.
Flash operations may silence output and restart the tone cycle.

Timing uses the board driver's declared 44118 Hz. Actual frequency and duration
depend on the hardware clock and require measurement. The public nominal rate
44100 differs from the board definition. No wall clock or synchronization is used.
The sine is hard-gated at each boundary; no additional envelope is applied.
A quarter-wave table (65 entries) keeps SRAM use small: upstream leaves only
345 bytes of app image headroom, and a full 256-entry table overflows it.
This build leaves 245 bytes. Further features need space freed elsewhere.

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

## Host verification
    gcc -std=c11 -Wall -Wextra -Werror -DFM1_HOST_TEST -Isdk/include -Iapp app/audio_output.c tests/tone_host.c -o tone-test
    ./tone-test

Checks the actual C renderer, including crossing frequency, silence boundaries
across blocks, amplitude, matching channels, zero master and input isolation.
These checks do not validate PI32 compilation, timing on hardware or flashing.

Modified: app/audio_output.c, app/audio_output.h
Added: tests/tone_host.c, TONE-README.md, tools/build_linux.py,
tools/linux-toolchain.json
Retain upstream licensing and SDK notices.
