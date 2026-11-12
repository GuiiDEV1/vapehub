import struct, math, os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vapehub.ico")
SIZES = [64, 48, 32, 16]
SS = 4

BG = (14, 16, 21)
BORDER = (48, 53, 66)
LEFT = (155, 123, 255)
RIGHT = (76, 224, 179)


def seg_dist(px, py, ax, ay, bx, by):
    vx, vy = bx - ax, by - ay
    wx, wy = px - ax, py - ay
    t = 0.0 if (vx * vx + vy * vy) == 0 else max(0.0, min(1.0, (wx * vx + wy * vy) / (vx * vx + vy * vy)))
    dx, dy = px - (ax + vx * t), py - (ay + vy * t)
    return math.hypot(dx, dy)


def rounded_sdf(px, py, cx, cy, hw, hh, r):
    qx, qy = abs(px - cx) - (hw - r), abs(py - cy) - (hh - r)
    ax, ay = max(qx, 0.0), max(qy, 0.0)
    return math.hypot(ax, ay) + min(max(qx, qy), 0.0) - r


def render(n):
    w = n * SS
    px = [[(0, 0, 0, 0)] * w for _ in range(w)]
    unit = w / 64.0
    hw = 30.0 * unit
    r_out = 13.0 * unit
    r_in = 11.8 * unit
    ax, ay = 19.0 * unit, 17.0 * unit
    bx, by = 32.0 * unit, 47.0 * unit
    cx, cy = 45.0 * unit, 17.0 * unit
    stroke = 7.0 * unit
    for y in range(w):
        for x in range(w):
            xc, yc = x + 0.5, y + 0.5
            d = rounded_sdf(xc, yc, w / 2.0, w / 2.0, hw, hw, r_out)
            if d > 0:
                continue
            col = (*BORDER, 255)
            din = rounded_sdf(xc, yc, w / 2.0, w / 2.0, hw, hw, r_in)
            if din < 0:
                col = (*BG, 255)
            if seg_dist(xc, yc, ax, ay, bx, by) <= stroke / 2.0:
                col = (*LEFT, 255)
            if seg_dist(xc, yc, bx, by, cx, cy) <= stroke / 2.0:
                col = (*RIGHT, 255)
            px[y][x] = col
    out = [[(0, 0, 0, 0)] * n for _ in range(n)]
    for oy in range(n):
        for ox in range(n):
            r = g = b = a = 0
            for sy in range(SS):
                for sx in range(SS):
                    cr, cg, cb, ca = px[oy * SS + sy][ox * SS + sx]
                    if ca:
                        r += cr
                        g += cg
                        b += cb
                        a += ca
            cnt = SS * SS
            if a:
                out[oy][ox] = (r // (a // 255), g // (a // 255), b // (a // 255), a // cnt)
    return out


def frame_bmp(pix, n):
    row = ((n + 31) // 32) * 4
    data = bytearray()
    for y in range(n - 1, -1, -1):
        for x in range(n):
            r, g, b, a = pix[y][x]
            data += bytes((b, g, r, a))
    data += b"\x00" * (row * n)
    hdr = struct.pack("<IiiHHIIiiII", 40, n, n * 2, 1, 32, 0, len(data), 0, 0, 0, 0)
    return hdr + bytes(data)


frames = []
for n in SIZES:
    frames.append((n, frame_bmp(render(n), n)))

hdr = struct.pack("<HHH", 0, 1, len(frames))
off = 6 + 16 * len(frames)
entries = b""
body = b""
for n, f in frames:
    entries += struct.pack("<BBBBHHII", n if n < 256 else 0, n if n < 256 else 0, 0, 0, 1, 32, len(f), off)
    body += f
    off += len(f)
with open(OUT, "wb") as fh:
    fh.write(hdr + entries + body)
print("wrote", OUT, os.path.getsize(OUT), "bytes")
