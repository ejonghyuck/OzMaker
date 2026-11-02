"""Generates res/app.ico (gradient squircle + record ring) with the standard library only."""
import math
import struct
import sys
import zlib
from pathlib import Path

C0 = (0x8B, 0x6C, 0xFF)  # violet
C1 = (0xFF, 0x5E, 0x87)  # pink


def render(size: int) -> bytes:
    ss = 4  # supersampling
    rad = size * 0.26
    rows = []
    for y in range(size):
        row = bytearray()
        for x in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(ss):
                for sx in range(ss):
                    px = x + (sx + 0.5) / ss
                    py = y + (sy + 0.5) / ss
                    # rounded square coverage
                    m = size * 0.04
                    qx = max(m + rad - px, 0, px - (size - m - rad))
                    qy = max(m + rad - py, 0, py - (size - m - rad))
                    if math.hypot(qx, qy) > rad:
                        continue
                    t = min(1.0, max(0.0, (px + py) / (2 * size)))
                    col = [C0[i] + (C1[i] - C0[i]) * t for i in range(3)]
                    cx = cy = size / 2
                    d = math.hypot(px - cx, py - cy)
                    ring_r, ring_w, dot_r = size * 0.25, size * 0.075, size * 0.115
                    if abs(d - ring_r) <= ring_w / 2 or d <= dot_r:
                        col = [255, 255, 255]
                    acc[0] += col[0]
                    acc[1] += col[1]
                    acc[2] += col[2]
                    acc[3] += 1
            n = ss * ss
            a = acc[3] / n
            if acc[3] > 0:
                r, g, b = (acc[i] / acc[3] for i in range(3))
            else:
                r = g = b = 0
            row += bytes((int(r + 0.5), int(g + 0.5), int(b + 0.5), int(a * 255 + 0.5)))
        rows.append(bytes(row))
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def main(out: Path) -> None:
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    pngs = [render(s) for s in sizes]
    header = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = b""
    for s, png in zip(sizes, pngs):
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(png), offset)
        offset += len(png)
    out.write_bytes(header + entries + b"".join(pngs))
    (out.parent / "app_256.png").write_bytes(pngs[-1])


if __name__ == "__main__":
    main(Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent / "res" / "app.ico")
