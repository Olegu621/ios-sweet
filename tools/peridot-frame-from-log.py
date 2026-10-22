#!/usr/bin/env python3
"""Decode a checksummed sweetfbshot RGB332 console frame into a PNG.

Reads only the console file; needs no device access or third-party modules.
"""
import argparse
from pathlib import Path
import re
import struct
import zlib


def decode(text):
    frame = None
    completed = None
    pending = {}
    for line in text.splitlines():
        at = line.find('SWEET SHOT ')
        if at < 0:
            continue
        line = line[at:]
        begin = re.fullmatch(r'SWEET SHOT begin (\d+) (\d+) rgb332 power=([01])', line)
        if begin:
            width, height, power = map(int, begin.groups())
            if not (1 <= width <= 160 and 1 <= height <= 512):
                raise ValueError('invalid frame dimensions')
            frame = (width, height, power, {})
            pending = {}
            continue
        if frame is None:
            raise ValueError('frame record without begin')
        width, height, power, rows = frame
        chunked = re.fullmatch(r'SWEET SHOT row (\d+) ([PR]) (\d+)/(\d+) ([0-9a-f]+)', line)
        single = re.fullmatch(r'SWEET SHOT row (\d+) ([PR]) ([0-9a-f]+)', line)
        if chunked or single:
            if chunked:
                y, mode, index, total, payload = chunked.groups()
                index, total = int(index), int(total)
            else:
                y, mode, payload = single.groups()
                index, total = 0, 1
            y = int(y)
            if y >= height or y in rows or len(payload) > width * 4:
                raise ValueError('invalid or duplicate row')
            part = pending.setdefault(y, {'mode': mode, 'chunks': [], 'total': total})
            if part['mode'] != mode or part['total'] != total:
                raise ValueError('mixed row chunks')
            if index != len(part['chunks']):
                raise ValueError('out of order row chunk')
            part['chunks'].append(payload)
            if len(part['chunks']) != total:
                continue
            data = b''
            for hexpart in part['chunks']:
                data += bytes.fromhex(hexpart)
            if mode == 'R':
                if len(data) % 2 or any(n == 0 for n in data[::2]):
                    raise ValueError('invalid RLE run')
                if sum(data[::2]) != width:
                    raise ValueError('invalid RLE row width')
                data = b''.join(bytes([c]) * n for n, c in zip(data[::2], data[1::2]))
            if len(data) != width:
                raise ValueError('invalid row width')
            rows[y] = data
            continue
        end = re.fullmatch(r'SWEET SHOT end ([0-9a-f]{8})', line)
        if not end:
            raise ValueError('malformed frame record')
        if len(rows) != height:
            raise ValueError('incomplete frame')
        pixels = b''.join(rows[y] for y in range(height))
        checksum = 2166136261
        for c in pixels:
            checksum = ((checksum ^ c) * 16777619) & 0xffffffff
        if checksum != int(end[1], 16):
            raise ValueError('frame checksum mismatch')
        completed = width, height, power, pixels
        frame = None
    if frame is not None or completed is None:
        raise ValueError('no complete frame')
    return completed


def png(width, height, pixels):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))

    raw = bytearray()
    for y in range(height):
        raw.append(0)  # PNG filter: none
        for c in pixels[y * width:(y + 1) * width]:
            raw.extend((((c >> 5) & 7) * 255 // 7,
                        ((c >> 2) & 7) * 255 // 7, (c & 3) * 255 // 3))
    header = struct.pack('>2I5B', width, height, 8, 2, 0, 0, 0)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('console', type=Path)
    ap.add_argument('output', type=Path)
    args = ap.parse_args()
    try:
        width, height, power, pixels = decode(args.console.read_text(errors='replace'))
    except ValueError as e:
        ap.exit(1, f'{e}\n')
    args.output.write_bytes(png(width, height, pixels))
    print(f'{width}x{height}, display power={power}: {args.output}')


if __name__ == '__main__':
    main()
