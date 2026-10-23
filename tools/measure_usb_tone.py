#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
"""Measure the FM1 test tone through its USB recording stream.

The FM1 capture endpoint is asynchronous: the device clock sets how many
frames arrive. Fitting received frames against this host's clock (keep NTP
synchronized) gives the device's actual frame rate. The tone itself is
measured in frames: burst length, cycle period, frequency and continuity.
Combining both gives real-time durations and frequency.

    python3 -I tools/measure_usb_tone.py --device hw:5,0 --seconds 300

MASTER must be up. Requires arecord (alsa-utils) and numpy.
"""
import argparse, subprocess, sys, time
import numpy as np

NOMINAL_RATE = 44118  # app/audio_output.c TONE_RATE


def capture(device, seconds):
    command = ['arecord', '-q', '-D', device, '-f', 'S24_3LE', '-c', '2', '-r', '44100', '-t', 'raw']
    process = subprocess.Popen(command, stdout=subprocess.PIPE, bufsize=0)
    chunks, stamps, frames = [], [], 0
    start = time.monotonic()
    try:
        while time.monotonic() - start < seconds:
            data = process.stdout.read(6 * 1024)
            if not data:
                sys.exit('error: capture ended early; is the FM1 connected?')
            now = time.monotonic()
            chunks.append(data)
            frames += len(data) // 6
            stamps.append((now, frames))
    finally:
        process.terminate()
        process.wait()
    raw = np.frombuffer(b''.join(chunks)[:frames * 6], dtype=np.uint8).reshape(-1, 2, 3).astype(np.int32)
    pcm = raw[..., 0] | raw[..., 1] << 8 | raw[..., 2] << 16
    pcm = np.where(pcm & 0x800000, pcm - 0x1000000, pcm)
    return pcm, np.array(stamps)


def frame_rate(stamps):
    """Frames per host second. Reads arrive late by varying amounts, so fit
    the earliest arrivals: the lower envelope in time, i.e. the upper in frames."""
    t, n = stamps[:, 0] - stamps[0, 0], stamps[:, 1].astype(float)
    keep = t > 2.0  # skip stream start-up
    t, n = t[keep], n[keep]
    slope, offset = np.polyfit(t, n, 1)
    for _ in range(3):  # refit on points at or above the line
        upper = n - (slope * t + offset) >= -1.0
        slope, offset = np.polyfit(t[upper], n[upper], 1)
    residual = n - (slope * t + offset)
    return slope, float(np.std(residual[residual >= -1.0])), t[-1] - t[0]


def bursts(signal):
    """(start, end) frame indices of nonzero runs separated by >= 1000 zero frames."""
    nonzero = np.flatnonzero(signal)
    if not len(nonzero):
        return []
    gaps = np.flatnonzero(np.diff(nonzero) > 1000)
    starts = np.concatenate(([nonzero[0]], nonzero[gaps + 1]))
    ends = np.concatenate((nonzero[gaps], [nonzero[-1]])) + 1
    return list(zip(starts, ends))


def sine_fit(x):
    """Least-squares frequency (cycles/frame), amplitude and worst residual."""
    n = np.arange(len(x))
    spectrum = np.abs(np.fft.rfft(x * np.hanning(len(x)), 1 << 20))
    f = np.argmax(spectrum) / (1 << 20)
    for _ in range(4):  # Gauss-Newton refinement of frequency
        basis = np.column_stack((np.sin(2 * np.pi * f * n), np.cos(2 * np.pi * f * n)))
        (a, b), *_ = np.linalg.lstsq(basis, x, rcond=None)
        model = basis @ (a, b)
        derivative = 2 * np.pi * n * (a * np.cos(2 * np.pi * f * n) - b * np.sin(2 * np.pi * f * n))
        f += np.dot(derivative, x - model) / np.dot(derivative, derivative)
    basis = np.column_stack((np.sin(2 * np.pi * f * n), np.cos(2 * np.pi * f * n)))
    (a, b), *_ = np.linalg.lstsq(basis, x, rcond=None)
    return f, float(np.hypot(a, b)), float(np.max(np.abs(x - basis @ (a, b))))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--device', default='hw:5,0', help='ALSA capture device of the FM1')
    parser.add_argument('--seconds', type=float, default=300.0)
    parser.add_argument('--save', help='also store the capture as .npz for re-analysis')
    parser.add_argument('--load', help='analyse a capture stored with --save')
    args = parser.parse_args()
    if args.load:
        stored = np.load(args.load)
        pcm, stamps = stored['pcm'], stored['stamps']
    else:
        print(f'Capturing {args.seconds:.0f} s from {args.device}; leave MASTER alone...')
        pcm, stamps = capture(args.device, args.seconds)
        if args.save:
            np.savez_compressed(args.save, pcm=pcm, stamps=stamps)
    left, right = pcm[:, 0].astype(float), pcm[:, 1].astype(float)
    rate, jitter, span = frame_rate(stamps)
    print(f'\nFrames: {len(pcm)}  channels identical: {bool(np.array_equal(pcm[:, 0], pcm[:, 1]))}')
    print(f'Device frame rate: {rate:.2f} frames/host-second over {span:.0f} s '
          f'(arrival jitter {jitter:.0f} frames; nominal {NOMINAL_RATE}, USB label 44100)')
    found = bursts(left)
    complete = [(s, e) for s, e in found if s > 0 and e < len(left)]
    if len(complete) < 2:
        sys.exit('error: fewer than two complete tone bursts; is MASTER up?')
    # MASTER changes split or distort bursts: keep only full-length ones.
    typical = np.median([e - s for s, e in complete])
    good = [(s, e) for s, e in complete if abs(e - s - typical) <= 4]
    print(f'Bursts: {len(complete)} complete, {len(good)} full-length (median {typical:.0f} frames, '
          f'expected {NOMINAL_RATE} before USB resampling)')
    if len(good) < 2:
        sys.exit('error: too few undisturbed bursts; leave MASTER alone during capture')
    # Burst starts are quantised to whole frames (11 ppm per cycle); fit them
    # all against their cycle numbers for a sub-frame cycle period.
    starts = np.array([s for s, _ in good], dtype=float)
    cycle = np.round((starts - starts[0]) / (2 * typical))
    period, _ = np.polyfit(cycle, starts, 1)
    scatter = np.std(starts - np.polyval(np.polyfit(cycle, starts, 1), cycle))
    lengths = np.array([e - s for s, e in good], dtype=float)
    fits = [sine_fit(left[s:e]) for s, e in good]
    freq = np.array([f for f, _, _ in fits])
    peak = max(a for _, a, _ in fits)
    worst = max(r for _, _, r in fits)
    print(f'  cycle period: {period:.3f} frames (start scatter {scatter:.2f} frames)')
    print(f'  on length: mean {lengths.mean():.2f} frames')
    print(f'  tone: {np.median(freq):.9f} cycles/frame, spread {np.ptp(freq):.2e}')
    print(f'  peak {peak:.0f} of 8388607 ({20 * np.log10(peak / 8388607):.1f} dBFS); '
          f'worst residual from a pure sine {worst:.0f}')
    print('\nIn real time, using the measured frame rate:')
    print(f'  tone frequency: {np.median(freq) * rate:.4f} Hz')
    print(f'  tone on: {lengths.mean() / rate * 1000:.4f} ms')
    print(f'  cycle period: {period / rate * 1000:.5f} ms '
          f'(error {(period / rate - 2) * 1e6:+.2f} us per 2 s, '
          f'{(period / rate / 2 - 1) * 1e6:+.2f} ppm, {(period / rate / 2 - 1) * 86400:+.2f} s/day)')


if __name__ == '__main__':
    main()
