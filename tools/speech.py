#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
"""Speech clip harness for the time-signal station.

speech/clips.tsv lists every clip. Sources, in order of preference:
speech/recordings/<id>.wav (your voice, from `record`) and
build/speech/tts/<id>.wav (placeholders from `synth`).

    python3 -I tools/speech.py synth --piper <venv python> --voice-dir <dir>
    python3 -I tools/speech.py record --device default
    python3 -I tools/speech.py process
    python3 -I tools/speech.py preview 14:32
    python3 -I tools/speech.py bank
    python3 -I tools/speech.py status

`process` trims silence, matches loudness and resamples to the station's
8 kHz mono 16-bit format in build/speech/clips/. `preview` renders the end
of the minute before HH:MM as broadcast: standard tone, second ticks with
their protected zones, the announcement at second 52.5 and the minute or
hour marker. `bank` encodes the processed clips as the station's speech bank
(app/speech.h) in build/speech/bank.bin, plus a reference rendering that
tests/speech_host.c compares with the firmware player. Requires numpy and
scipy; `record` also needs arecord.
"""
import argparse, json, os, struct, subprocess, sys, zlib
from math import gcd
from pathlib import Path
import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / 'speech' / 'clips.tsv'
RECORDINGS = ROOT / 'speech' / 'recordings'
OUT = ROOT / 'build' / 'speech'
TTS, CLIPS = OUT / 'tts', OUT / 'clips'
CLIP_RATE = 8000           # stored speech rate
PREVIEW_RATE = 44100
SPEECH_RMS_DB = -20.0      # active-speech loudness target
SPEECH_PEAK_DB = -1.0
ANNOUNCE_AT = 52.5         # WWV voice announcement start (s)


def clips():
    entries = []
    for line in MANIFEST.read_text(encoding='utf-8').splitlines():
        if not line.strip() or line.startswith('#'):
            continue
        fields = line.split('\t')
        entries.append({'id': fields[0], 'text': fields[1],
                        'say': fields[2] if len(fields) > 2 and fields[2] else fields[1]})
    return entries


def source(clip_id):
    for path, kind in ((RECORDINGS / f'{clip_id}.wav', 'recording'), (TTS / f'{clip_id}.wav', 'tts')):
        if path.is_file():
            return path, kind
    return None, 'missing'


def read_mono(path):
    rate, data = wavfile.read(path)
    if data.dtype.kind in 'iu':
        data = data.astype(np.float64) / float(np.iinfo(data.dtype).max)
    data = np.asarray(data, dtype=np.float64)
    return rate, data.mean(axis=1) if data.ndim > 1 else data


def resample(x, rate, target):
    g = gcd(rate, target)
    return resample_poly(x, target // g, rate // g)


# Runs inside the Piper environment; loads the voice once for all clips.
PIPER_SCRIPT = r'''
import json, sys, wave
from piper import PiperVoice
job = json.load(sys.stdin)
voice = PiperVoice.load(job["model"])
for item in job["items"]:
    with wave.open(item["path"], "wb") as out:
        voice.synthesize_wav(item["say"], out)
    print(item["id"], flush=True)
'''


def synth(args):
    model = Path(args.voice_dir) / f'{args.voice}.onnx'
    if not model.is_file():
        sys.exit(f'error: voice model not found: {model}')
    TTS.mkdir(parents=True, exist_ok=True)
    items = [{'id': c['id'], 'say': c['say'], 'path': str(TTS / f"{c['id']}.wav")}
             for c in clips() if args.force or not (TTS / f"{c['id']}.wav").is_file()]
    if args.only:
        items = [i for i in items if i['id'] in args.only]
    if not items:
        print('All placeholder clips exist; use --force to regenerate.')
        return
    print(f'Synthesizing {len(items)} clips with {args.voice}...')
    job = json.dumps({'model': str(model), 'items': items})
    result = subprocess.run([args.piper, '-c', PIPER_SCRIPT], input=job, text=True)
    if result.returncode:
        sys.exit('error: Piper failed')
    (TTS / 'VOICE').write_text(args.voice + '\n')


def record(args):
    RECORDINGS.mkdir(parents=True, exist_ok=True)
    todo = [c for c in clips() if (not args.only or c['id'] in args.only) and
            (args.force or not (RECORDINGS / f"{c['id']}.wav").is_file())]
    print(f'{len(todo)} clips to record. Speak at a steady pace; leave a moment of silence at each end.')
    for index, clip in enumerate(todo, 1):
        path = RECORDINGS / f"{clip['id']}.wav"
        while True:
            print(f"\n[{index}/{len(todo)}] {clip['id']}:  {clip['text']}")
            if input('Enter to start recording (q to quit): ').strip().lower() == 'q':
                return
            process = subprocess.Popen(['arecord', '-q', '-D', args.device, '-f', 'S16_LE', '-c', '1',
                                        '-r', str(args.rate), str(path)])
            input('Recording... Enter to stop.')
            process.terminate()
            process.wait()
            rate, x = read_mono(path)
            print(f'  {len(x) / rate:.2f} s, peak {20 * np.log10(max(np.max(np.abs(x)), 1e-9)):.1f} dBFS')
            if input('Enter to keep, r to redo: ').strip().lower() != 'r':
                break


def trim(x, rate):
    window = max(1, rate // 100)  # 10 ms
    energy = np.sqrt(np.convolve(x * x, np.ones(window) / window, mode='same'))
    active = np.flatnonzero(energy > energy.max() * 10 ** (-40 / 20))
    if not len(active):
        return x[:0]
    pad = rate * 15 // 1000
    return x[max(0, active[0] - pad):min(len(x), active[-1] + pad)]


def level(x):
    loud = np.abs(x) > np.max(np.abs(x)) * 10 ** (-30 / 20)
    gain = 10 ** (SPEECH_RMS_DB / 20) / max(np.sqrt(np.mean(x[loud] ** 2)), 1e-9)
    gain = min(gain, 10 ** (SPEECH_PEAK_DB / 20) / max(np.max(np.abs(x)), 1e-9))
    return x * gain


def process(args):
    CLIPS.mkdir(parents=True, exist_ok=True)
    total, missing = 0.0, []
    for clip in clips():
        path, kind = source(clip['id'])
        if not path:
            missing.append(clip['id'])
            continue
        rate, x = read_mono(path)
        y = level(resample(trim(x, rate), rate, CLIP_RATE))
        wavfile.write(CLIPS / f"{clip['id']}.wav", CLIP_RATE, np.round(y * 32767).astype(np.int16))
        total += len(y) / CLIP_RATE
    print(f'Processed {len(clips()) - len(missing)} clips: {total:.1f} s of speech, '
          f'{total * CLIP_RATE * 2 / 1024:.0f} KiB as 16-bit PCM, ~{total * CLIP_RATE / 2 / 1024:.0f} KiB as 4-bit ADPCM')
    if missing:
        print('Missing:', ' '.join(missing))


def load_clip(clip_id):
    path = CLIPS / f'{clip_id}.wav'
    if not path.is_file():
        sys.exit(f'error: {path} missing; run process first')
    rate, x = read_mono(path)
    return resample(x, rate, PREVIEW_RATE)


def announcement(hour, minute):
    """Clip ids and the silence (s) after each: the time at the next marker."""
    return [('at_the_tone', 0.25), (f'n{hour:02d}', 0.06), ('hour' if hour == 1 else 'hours', 0.2),
            (f'n{minute:02d}', 0.06), ('minute' if minute == 1 else 'minutes', 0.2), ('zone', 0.0)]


def standard_tone(minute):
    """WWV schedule for seconds 1-45 of a minute: Hz, or 0 for silence."""
    if minute in (0, 3, 29, 30, 59) or 43 <= minute <= 51:
        return 0
    if minute == 2:
        return 440
    return 500 if minute % 2 == 0 else 600


def preview(args):
    hour, minute = (int(v) for v in args.time.split(':'))
    if not (0 <= hour < 24 and 0 <= minute < 60):
        sys.exit('error: time must be HH:MM, 24-hour')
    previous = (minute - 1) % 60  # the minute whose end carries this announcement
    start, end = 43.0, 61.2
    t = np.arange(int((end - start) * PREVIEW_RATE)) / PREVIEW_RATE + start
    out = np.zeros_like(t)
    tone = standard_tone(previous)
    if tone:
        out += np.where((t >= 1) & (t < 45), 0.3 * np.sin(2 * np.pi * tone * t), 0)
    voice_at = int((ANNOUNCE_AT - start) * PREVIEW_RATE)
    at, words = voice_at, []
    for clip_id, gap in announcement(hour, minute):
        x = load_clip(clip_id)
        out[at:at + len(x)] += x[:len(out) - at]
        words.append(clip_id)
        at += len(x) + int(gap * PREVIEW_RATE)
    voice_end = start + at / PREVIEW_RATE
    # Protected zone: nothing but the tick from 10 ms before to 30 ms after each second.
    for second in range(int(np.ceil(start)), int(end) + 1):
        if second % 60 in (29, 59):
            continue
        zone = (t >= second - 0.010) & (t < second + 0.030)
        out[zone] = 0
        if second != 60:
            tick = (t >= second) & (t < second + 0.005)
            out[tick] = 0.9 * np.sin(2 * np.pi * 1000 * (t[tick] - second))
    marker = (t >= 60) & (t < 60.8)
    out[marker] = 0.9 * np.sin(2 * np.pi * (1500 if minute == 0 else 1000) * (t[marker] - 60))
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / f'preview_{hour:02d}{minute:02d}.wav'
    wavfile.write(path, PREVIEW_RATE, np.round(np.clip(out, -1, 1) * 32767).astype(np.int16))
    print(f'{path}: seconds {start:.0f}-{end:.1f} of minute :{previous:02d}, '
          f'tone {f"{tone} Hz" if tone else "none"}, marker {1500 if minute == 0 else 1000} Hz')
    print(f'  {" ".join(words)}')
    print(f'  voice {ANNOUNCE_AT:.1f}-{voice_end:.2f} s'
          + ('' if voice_end <= 59.9 else '  WARNING: runs into the minute marker'))


# IMA ADPCM, mirroring app/speech.c exactly.
IMA_STEPS = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
             50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
             253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
             1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
             3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
             11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]
IMA_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]
BANK_RATE, BOARD_RATE = 8000, 44118
RESAMPLE_STEP = round(BANK_RATE * 2 ** 32 / BOARD_RATE)


def ima_step(predictor, index, n):
    step = IMA_STEPS[index]
    diff = step >> 3
    if n & 4: diff += step
    if n & 2: diff += step >> 1
    if n & 1: diff += step >> 2
    predictor = max(-32768, min(32767, predictor - diff if n & 8 else predictor + diff))
    return predictor, max(0, min(88, index + IMA_INDEX[n & 7]))


def ima_encode(samples):
    predictor, index, nibbles = 0, 0, []
    for x in samples:
        step, diff, n = IMA_STEPS[index], int(x) - predictor, 0
        if diff < 0: n, diff = 8, -diff
        for bit, threshold in ((4, step), (2, step >> 1), (1, step >> 2)):
            if diff >= threshold:
                n |= bit
                diff -= threshold
        nibbles.append(n)
        predictor, index = ima_step(predictor, index, n)
    if len(nibbles) % 2:
        nibbles.append(0)
    return bytes(nibbles[i] | nibbles[i + 1] << 4 for i in range(0, len(nibbles), 2))


def bank_clip_ids():
    return [c['id'] for c in clips()]


def build_bank():
    entries, payload = [], b''
    ids = bank_clip_ids()
    if any(len(i) > 12 for i in ids):
        sys.exit('error: clip ids must be at most 12 characters')
    base = 32 + 24 * len(ids)
    for clip_id in ids:
        path = CLIPS / f'{clip_id}.wav'
        if not path.is_file():
            sys.exit(f'error: {path} missing; run process first')
        rate, x = wavfile.read(path)
        if rate != BANK_RATE or x.dtype != np.int16:
            sys.exit(f'error: {path} is not {BANK_RATE} Hz 16-bit; run process')
        data = ima_encode(x)
        entries.append((clip_id, base + len(payload), len(x)))
        payload += data
    directory = b''.join(struct.pack('<12sIIhBB', i.encode(), offset, samples, 0, 0, 0)
                         for i, offset, samples in entries)
    body = struct.pack('<IH10x', 32 + len(directory) + len(payload), len(ids)) + directory + payload
    header = b'HSTS' + struct.pack('<BBHI', 1, 1, BANK_RATE, 0) + struct.pack('<I', zlib.crc32(body))
    return header + body


def render(bank, playlist):
    """Board-rate samples for [(clip id, gap frames)], as speech_block plays them."""
    count = struct.unpack_from('<H', bank, 20)[0]
    directory = {}
    for i in range(count):
        name, offset, samples, predictor, index, _ = struct.unpack_from('<12sIIhBB', bank, 32 + 24 * i)
        directory[name.rstrip(b'\0').decode()] = (offset, samples, predictor, index)
    out = []
    for clip_id, gap in playlist:
        offset, remaining, predictor, index = directory[clip_id]
        previous = current = predictor
        fraction, position, high = 0, offset, 0
        while True:
            before = fraction
            fraction = (fraction + RESAMPLE_STEP) & 0xffffffff
            if fraction < before:
                previous = current
                if not remaining:
                    out.extend([0] * (gap + 1))
                    break
                remaining -= 1
                byte = bank[position]
                n = byte >> 4 if high else byte & 15
                position += high
                high ^= 1
                predictor, index = ima_step(predictor, index, n)
                current = predictor
            out.append(previous + (((current - previous) * (fraction >> 18)) >> 14))
    return np.array(out, dtype=np.int16)


def gap_frames(ms):
    return ms * BOARD_RATE // 1000


def bank(args):
    data = build_bank()
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / 'bank.bin').write_bytes(data)
    if len(data) > 466944:
        sys.exit(f'error: bank is {len(data)} bytes; the limit is 466944')
    # Reference: the announcement armed at 14:31:52.5 local, for 14:32.
    playlist = [('at_the_tone', gap_frames(250)), ('n14', gap_frames(60)), ('hours', gap_frames(200)),
                ('n32', gap_frames(60)), ('minutes', gap_frames(200)), ('zone', 0)]
    reference = render(data, playlist)
    (OUT / 'bank_reference_1432.raw').write_bytes(reference.tobytes())
    print(f'{OUT / "bank.bin"}: {len(data)} bytes, {len(bank_clip_ids())} clips '
          f'({len(data) / 466944:.0%} of the speech region)')
    print(f'  reference announcement: {len(reference) / BOARD_RATE:.2f} s')


def status(args):
    durations = []
    for clip in clips():
        path, kind = source(clip['id'])
        out = CLIPS / f"{clip['id']}.wav"
        length = ''
        if out.is_file():
            rate, x = read_mono(out)
            length = f'{len(x) / rate:5.2f} s'
            durations.append(len(x) / rate)
        print(f"{clip['id']:12} {kind:9} {length:8} {clip['text']}")
    if durations:
        print(f'{len(durations)} processed, {sum(durations):.1f} s total')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest='command', required=True)
    p = commands.add_parser('synth', help='generate placeholder clips with Piper')
    p.add_argument('--piper', default=os.environ.get('FM1_PIPER_PYTHON'),
                   help='python of an environment with piper-tts installed')
    p.add_argument('--voice-dir', default=os.environ.get('FM1_PIPER_VOICES'))
    p.add_argument('--voice', default='en_US-john-medium')
    p.add_argument('--force', action='store_true')
    p.add_argument('--only', nargs='+')
    p = commands.add_parser('record', help='record your own clips')
    p.add_argument('--device', default='default', help='ALSA capture device')
    p.add_argument('--rate', type=int, default=48000)
    p.add_argument('--force', action='store_true')
    p.add_argument('--only', nargs='+')
    commands.add_parser('process', help='trim, level and resample clips to 8 kHz')
    p = commands.add_parser('preview', help='render the end of the minute before HH:MM')
    p.add_argument('time')
    commands.add_parser('bank', help='encode processed clips as the station speech bank')
    commands.add_parser('status', help='list clips and their sources')
    args = parser.parse_args()
    if args.command == 'synth' and (not args.piper or not args.voice_dir):
        sys.exit('error: set --piper and --voice-dir (or FM1_PIPER_PYTHON and FM1_PIPER_VOICES)')
    {'synth': synth, 'record': record, 'process': process, 'preview': preview, 'bank': bank,
     'status': status}[args.command](args)


if __name__ == '__main__':
    main()
