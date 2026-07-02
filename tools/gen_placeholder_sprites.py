#!/usr/bin/env python3
"""Generate 8-direction placeholder character sprite-sheets (no dependencies).

Each sheet is a horizontal strip of 8 frames (64x64 each -> 512x64 RGBA PNG).
Frame i corresponds to facing angle i*45 degrees, ordered CCW from +X (East),
with +Y up (screen up):

    i = 0:E  1:NE  2:N  3:NW  4:W  5:SW  6:S  7:SE

So the runtime picks a frame with:
    frame = (round(atan2(dir.y, dir.x) / (pi/4)) + 8) % 8

Each frame draws a round body + a bright "beak" pointing in the facing direction
(the orientation cue) + two eyes. Player and enemy differ only in palette. These
are debug placeholders; when Godot becomes the frontend they stand in for a
missing Godot sprite.

Run from the repo root:
    python3 tools/gen_placeholder_sprites.py
"""

import math
import struct
import zlib
from pathlib import Path

FRAME = 64
FRAMES = 8
SHEET_W = FRAME * FRAMES
SHEET_H = FRAME
RADIUS = 24.0


def _png_chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data +
            struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def write_png(path, width, height, rgba):
    # rgba: bytearray of width*height*4, straight alpha.
    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)  # filter type 0 (None) per scanline
        raw.extend(rgba[y * stride:(y + 1) * stride])
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" +
           _png_chunk(b"IHDR", ihdr) +
           _png_chunk(b"IDAT", zlib.compress(bytes(raw), 9)) +
           _png_chunk(b"IEND", b""))
    Path(path).write_bytes(png)


class Canvas:
    def __init__(self, w, h):
        self.w = w
        self.h = h
        self.buf = bytearray(w * h * 4)  # transparent black

    def put(self, x, y, rgba):
        xi, yi = int(x), int(y)
        if 0 <= xi < self.w and 0 <= yi < self.h:
            o = (yi * self.w + xi) * 4
            self.buf[o:o + 4] = bytes(rgba)

    def disc(self, cx, cy, r, rgba):
        for y in range(int(cy - r) - 1, int(cy + r) + 2):
            for x in range(int(cx - r) - 1, int(cx + r) + 2):
                if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                    self.put(x, y, rgba)

    def ring(self, cx, cy, r_out, r_in, rgba):
        for y in range(int(cy - r_out) - 1, int(cy + r_out) + 2):
            for x in range(int(cx - r_out) - 1, int(cx + r_out) + 2):
                d2 = (x - cx) ** 2 + (y - cy) ** 2
                if r_in * r_in <= d2 <= r_out * r_out:
                    self.put(x, y, rgba)

    def tri(self, ax, ay, bx, by, cx, cy, rgba):
        minx = int(min(ax, bx, cx)) - 1
        maxx = int(max(ax, bx, cx)) + 1
        miny = int(min(ay, by, cy)) - 1
        maxy = int(max(ay, by, cy)) + 1

        def edge(x0, y0, x1, y1, px, py):
            return (px - x0) * (y1 - y0) - (py - y0) * (x1 - x0)

        for y in range(miny, maxy + 1):
            for x in range(minx, maxx + 1):
                px, py = x + 0.5, y + 0.5
                w0 = edge(bx, by, cx, cy, px, py)
                w1 = edge(cx, cy, ax, ay, px, py)
                w2 = edge(ax, ay, bx, by, px, py)
                if (w0 >= 0 and w1 >= 0 and w2 >= 0) or \
                   (w0 <= 0 and w1 <= 0 and w2 <= 0):
                    self.put(x, y, rgba)


def build_sheet(body, outline, beak, eye):
    c = Canvas(SHEET_W, SHEET_H)
    cy = FRAME / 2.0
    for i in range(FRAMES):
        theta = math.radians(i * 45.0)
        # Facing in image space: world +Y is up, image y is down -> flip y.
        dx, dy = math.cos(theta), -math.sin(theta)
        px, py = -dy, dx  # perpendicular
        cx = i * FRAME + FRAME / 2.0

        # Body + outline ring.
        c.disc(cx, cy, RADIUS, body)
        c.ring(cx, cy, RADIUS, RADIUS - 2.5, outline)

        # Beak: a triangle sticking out past the body in the facing direction.
        tipx, tipy = cx + dx * (RADIUS + 6), cy + dy * (RADIUS + 6)
        b0x, b0y = cx + dx * 4 + px * 10, cy + dy * 4 + py * 10
        b1x, b1y = cx + dx * 4 - px * 10, cy + dy * 4 - py * 10
        c.tri(tipx, tipy, b0x, b0y, b1x, b1y, beak)

        # Two eyes offset toward the facing edge.
        for s in (-1.0, 1.0):
            ex = cx + dx * 9 + px * 6 * s
            ey = cy + dy * 9 + py * 6 * s
            c.disc(ex, ey, 2.4, eye)
    return c


def main():
    out = Path(__file__).resolve().parent.parent / "game" / "data" / "base" / "textures"
    out.mkdir(parents=True, exist_ok=True)

    # Player: cyan-blue body, white beak.
    player = build_sheet(
        body=(64, 150, 235, 255),
        outline=(18, 42, 84, 255),
        beak=(240, 246, 255, 255),
        eye=(16, 24, 40, 255),
    )
    write_png(out / "placeholder_player_8dir.png", SHEET_W, SHEET_H, player.buf)

    # Enemy / non-player: red-orange body, pale-yellow beak.
    enemy = build_sheet(
        body=(210, 72, 55, 255),
        outline=(74, 20, 18, 255),
        beak=(255, 226, 158, 255),
        eye=(32, 12, 10, 255),
    )
    write_png(out / "placeholder_enemy_8dir.png", SHEET_W, SHEET_H, enemy.buf)

    print("wrote", out / "placeholder_player_8dir.png")
    print("wrote", out / "placeholder_enemy_8dir.png")


if __name__ == "__main__":
    main()
