#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
"""Set and query the time-signal station over USB MIDI SysEx.

    python3 -I tools/station.py sync [--offset -8:48] [--rate 44117.66]
    python3 -I tools/station.py query
    python3 -I tools/station.py offset -8:48
    python3 -I tools/station.py rate 44117.66
    python3 -I tools/station.py upload build/speech/bank.bin

`sync` sends this host's UTC time of day; keep the host NTP-synchronized.
Protocol: app/transfer.c. The device is found by name in /proc/asound/cards.
"""
import argparse, errno, os, re, select, sys, time
from pathlib import Path

HEADER = bytes([0xf0, 0x7d, ord('T'), ord('S'), 1])
SET_TIME, SET_OFFSET, SET_RATE, QUERY, BANK_BEGIN, BANK_WRITE, BANK_FINISH = 1, 2, 3, 4, 5, 6, 7
DAY_MS = 86400 * 1000
CHUNK = 256
BANK_STATES = ['no speech bank', 'checking', 'speech bank ready', 'uploading', 'speech bank damaged']


def device_path(name):
    for line in Path('/proc/asound/cards').read_text().splitlines():
        m = re.match(r'\s*(\d+)\s+\[.*\]:.*- (.*)$', line)
        if m and name.lower() in m.group(2).lower():
            path = f'/dev/snd/midiC{m.group(1)}D0'
            if os.path.exists(path):
                return path
    sys.exit(f'error: no MIDI device named like "{name}"; is the FM1 connected?')


def pack7(value, groups):
    return bytes((value >> (7 * i)) & 127 for i in range(groups))


def unpack7(data):
    return sum((b & 127) << (7 * i) for i, b in enumerate(data))


def pack_stream(data):
    """8-bit bytes as an LSB-first stream of 7-bit MIDI bytes."""
    out, value, bits = bytearray(), 0, 0
    for byte in data:
        value |= byte << bits
        bits += 8
        while bits >= 7:
            out.append(value & 127)
            value >>= 7
            bits -= 7
    if bits:
        out.append(value & 127)
    return bytes(out)


def utc_ms_of_day():
    return int(time.time() * 1000) % DAY_MS


def transact(fd, command, payload=b'', timed=False, latency_ms=0, timeout=1.0):
    """Send one command and return the decoded reply. A timed SET_TIME takes
    its timestamp immediately before writing."""
    while select.select([fd], [], [], 0)[0]:
        os.read(fd, 256)  # discard stale input
    if timed:
        sent = utc_ms_of_day()
        payload = pack7((sent + latency_ms) % DAY_MS, 4)
    os.write(fd, HEADER + bytes([command]) + payload + b'\xf7')
    buffer, deadline = b'', time.monotonic() + timeout
    while time.monotonic() < deadline:
        if select.select([fd], [], [], 0.05)[0]:
            buffer += os.read(fd, 256)
            start = buffer.find(HEADER + bytes([command | 0x40]))
            end = buffer.find(b'\xf7', start + 1) if start >= 0 else -1
            if end > 0:
                reply = buffer[start + len(HEADER) + 1:end]
                host = utc_ms_of_day()
                return {'rejected': reply[0], 'utc_ms': unpack7(reply[1:5]),
                        'offset': unpack7(reply[5:7]) - 1024, 'rate': unpack7(reply[7:11]),
                        'set': bool(reply[11] & 1), 'bank': reply[12], 'clips': unpack7(reply[13:15]),
                        'bank_bytes': unpack7(reply[15:19]), 'host_ms': host}
    sys.exit('error: no reply from the station (is the station firmware running?)')


def hms(ms):
    s = ms // 1000 % 86400
    return f'{s // 3600:02d}:{s // 60 % 60:02d}:{s % 60:02d}.{ms % 1000:03d}'


def show(reply, label):
    if reply['rejected']:
        sys.exit(f'error: station rejected {label}')
    local = (reply['utc_ms'] + reply['offset'] * 60000) % DAY_MS
    sign = '-' if reply['offset'] < 0 else '+'
    hours, minutes = divmod(abs(reply['offset']), 60)
    print(f"{label}: UTC {hms(reply['utc_ms'])}  local {hms(local)}  (UTC{sign}{hours}:{minutes:02d})  "
          f"rate {reply['rate'] / 1000:.3f} Hz  {'set' if reply['set'] else 'NOT SET'}")
    if reply['set']:
        delta = (reply['utc_ms'] - reply['host_ms'] + DAY_MS // 2) % DAY_MS - DAY_MS // 2
        print(f'  station minus host: {delta:+d} ms (includes USB and reply latency)')
    state = BANK_STATES[reply['bank']] if reply['bank'] < len(BANK_STATES) else f"bank state {reply['bank']}"
    detail = f": {reply['clips']} clips, {reply['bank_bytes']} bytes" if reply['bank'] == 2 else ''
    print(f'  {state}{detail}')


def upload(fd, path):
    data = Path(path).read_bytes()
    if data[:4] != b'HSTS':
        sys.exit(f'error: {path} is not a speech bank; build it with tools/speech.py bank')
    print(f'Uploading {len(data)} bytes; the station is briefly silent while Flash is written.')
    if transact(fd, BANK_BEGIN, pack7(len(data), 4), timeout=3.0)['rejected']:
        sys.exit('error: station rejected the bank size')
    started = time.monotonic()
    for offset in range(0, len(data), CHUNK):
        chunk = data[offset:offset + CHUNK]
        if transact(fd, BANK_WRITE, pack7(offset, 4) + pack_stream(chunk), timeout=3.0)['rejected']:
            sys.exit(f'error: station rejected bytes at {offset}')
        done = offset + len(chunk)
        print(f'\r  {done}/{len(data)} bytes ({done / max(time.monotonic() - started, 1e-3) / 1024:.1f} KiB/s)',
              end='', flush=True)
    print()
    reply = transact(fd, BANK_FINISH, timeout=5.0)
    if reply['rejected']:
        sys.exit('error: the uploaded bank failed validation; nothing was committed')
    show(reply, 'upload')
    # Flash writes pause the audio-clocked station clock; restore the time.
    if reply['set']:
        show(transact(fd, SET_TIME, timed=True), 'resync')


def offset_minutes(text):
    m = re.fullmatch(r'([+-]?)(\d{1,2})(?::(\d{2}))?', text)
    if not m:
        raise argparse.ArgumentTypeError('use [+-]H[:MM], e.g. -8:00')
    minutes = int(m.group(2)) * 60 + int(m.group(3) or 0)
    return -minutes if m.group(1) == '-' else minutes


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--device', default='FM1', help='ALSA card name to match')
    commands = parser.add_subparsers(dest='command', required=True)
    p = commands.add_parser('sync', help='set the station to this host\'s UTC time')
    p.add_argument('--offset', type=offset_minutes, help='also set the UTC offset, e.g. -8:00')
    p.add_argument('--rate', type=float, help='also set the audio frame rate in Hz, e.g. 44117.66')
    p.add_argument('--latency-ms', type=int, default=0, help='add to the time sent (calibration)')
    commands.add_parser('query', help='read the station clock')
    p = commands.add_parser('offset', help='set the UTC offset')
    p.add_argument('value', type=offset_minutes)
    p = commands.add_parser('rate', help='set the audio frame rate in Hz')
    p.add_argument('value', type=float)
    p = commands.add_parser('upload', help='upload a speech bank from tools/speech.py bank')
    p.add_argument('bank')
    args = parser.parse_args()

    path = device_path(args.device)
    try:
        fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
    except OSError as error:
        if error.errno == errno.EBUSY:
            sys.exit(f'error: {path} is in use; close the workbench page or other MIDI software')
        raise
    try:
        if args.command in ('offset', 'sync') and getattr(args, 'offset' if args.command == 'sync' else 'value') is not None:
            minutes = args.offset if args.command == 'sync' else args.value
            show(transact(fd, SET_OFFSET, pack7(minutes + 1024, 2)), 'offset')
        if args.command in ('rate', 'sync') and getattr(args, 'rate' if args.command == 'sync' else 'value') is not None:
            hz = args.rate if args.command == 'sync' else args.value
            show(transact(fd, SET_RATE, pack7(round(hz * 1000), 4)), 'rate')
        if args.command == 'sync':
            show(transact(fd, SET_TIME, timed=True, latency_ms=args.latency_ms), 'sync')
            time.sleep(0.2)
            show(transact(fd, QUERY), 'query')
        elif args.command == 'query':
            show(transact(fd, QUERY), 'query')
        elif args.command == 'upload':
            upload(fd, args.bank)
    finally:
        os.close(fd)


if __name__ == '__main__':
    main()
