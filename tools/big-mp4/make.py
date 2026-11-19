#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A valid MP4 bigger than 4 GiB with its index at the end, for testing large-file playback (32-bit offsets).

    make.py SMALL.mp4 OUT.mp4 [SIZE_GB]

Layout: ftyp, then one mdat with a 64-bit size holding SMALL's media data followed by sparse zero padding (no disk
space used), then SMALL's moov with its chunk offsets moved to the new mdat. A player must read past 4 GiB to find
the moov, as with real long 4K files (moov at the end). Only top-level atoms and stco/co64 are rewritten.
"""
import struct
import sys

CONTAINERS = {b"moov", b"trak", b"mdia", b"minf", b"stbl", b"edts", b"dinf", b"udta"}


def atoms(data, start=0, end=None):
    end = len(data) if end is None else end
    pos = start
    while pos + 8 <= end:
        size, kind = struct.unpack(">I4s", data[pos:pos + 8])
        header = 8
        if size == 1:
            size = struct.unpack(">Q", data[pos + 8:pos + 16])[0]
            header = 16
        elif size == 0:
            size = end - pos
        yield pos, size, header, kind
        pos += size


def shift_offsets(moov, delta):
    moov = bytearray(moov)

    def walk(start, end):
        for pos, size, header, kind in atoms(moov, start, end):
            if kind in CONTAINERS:
                walk(pos + header, pos + size)
            elif kind == b"stco":
                n = struct.unpack(">I", moov[pos + 12:pos + 16])[0]
                for i in range(n):
                    o = pos + 16 + 4 * i
                    struct.pack_into(">I", moov, o, struct.unpack(">I", moov[o:o + 4])[0] + delta)
            elif kind == b"co64":
                n = struct.unpack(">I", moov[pos + 12:pos + 16])[0]
                for i in range(n):
                    o = pos + 16 + 8 * i
                    struct.pack_into(">Q", moov, o, struct.unpack(">Q", moov[o:o + 8])[0] + delta)

    walk(8, len(moov))
    return bytes(moov)


def main():
    src, out = sys.argv[1], sys.argv[2]
    size_gb = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0
    data = open(src, "rb").read()
    top = {kind: (pos, size, header) for pos, size, header, kind in atoms(data)}
    ftyp_pos, ftyp_size, _ = top[b"ftyp"]
    mdat_pos, mdat_size, mdat_header = top[b"mdat"]
    moov_pos, moov_size, _ = top[b"moov"]
    ftyp = data[ftyp_pos:ftyp_pos + ftyp_size]
    payload = data[mdat_pos + mdat_header:mdat_pos + mdat_size]
    new_payload_start = len(ftyp) + 16
    padding = int(size_gb * 1024 ** 3) - len(ftyp) - 16 - len(payload) - moov_size
    new_mdat_size = 16 + len(payload) + padding
    moov = shift_offsets(data[moov_pos:moov_pos + moov_size], new_payload_start - (mdat_pos + mdat_header))
    with open(out, "wb") as f:
        f.write(ftyp)
        f.write(struct.pack(">I4sQ", 1, b"mdat", new_mdat_size))
        f.write(payload)
        f.seek(len(ftyp) + new_mdat_size)  # sparse zeros up to here
        f.write(moov)
    total = len(ftyp) + new_mdat_size + len(moov)
    print(f"{out}: {total} bytes ({total / 1024 ** 3:.2f} GiB), moov at {len(ftyp) + new_mdat_size}")


if __name__ == "__main__":
    main()
