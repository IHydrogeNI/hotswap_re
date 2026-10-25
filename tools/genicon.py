# Generates src/hotswap_re.ico (16, 20, 24, 32, 40, 48 px, 32-bit BGRA) - a drive with a green swap arrow.
# usage: python genicon.py ../src/hotswap_re.ico
import struct, sys

def render(n):
    ss = 4
    def sample(x, y):  # x, y in 0..1 ; returns (r, g, b, a)
        # drive body: rounded rect
        bx0, by0, bx1, by1, r = 0.06, 0.40, 0.94, 0.92, 0.10
        def in_rrect(x, y, x0, y0, x1, y1, r):
            cx = min(max(x, x0 + r), x1 - r); cy = min(max(y, y0 + r), y1 - r)
            return (x - cx) ** 2 + (y - cy) ** 2 <= r * r and x0 <= x <= x1 and y0 <= y <= y1
        # green up arrow (eject / swap)
        ax, aw = 0.5, 0.30
        if 0.03 <= y <= 0.24 and abs(x - ax) <= aw * (y - 0.03) / 0.21:
            return (46, 170, 80, 255)
        if 0.24 <= y <= 0.34 and abs(x - ax) <= 0.11:
            return (46, 170, 80, 255)
        if in_rrect(x, y, bx0, by0, bx1, by1, r):
            if not in_rrect(x, y, bx0 + 0.05, by0 + 0.05, bx1 - 0.05, by1 - 0.05, r * 0.6):
                return (40, 48, 60, 255)  # outline
            if (x - 0.78) ** 2 + (y - 0.66) ** 2 <= 0.06 ** 2:
                return (90, 220, 120, 255)  # LED
            if 0.16 <= x <= 0.58 and 0.62 <= y <= 0.70:
                return (70, 80, 96, 255)    # slot
            t = (y - by0) / (by1 - by0)
            g = int(200 - 50 * t)
            return (g, g + 6, g + 14, 255)
        return (0, 0, 0, 0)
    px = []
    for j in range(n):
        row = []
        for i in range(n):
            acc = [0, 0, 0, 0]
            for sj in range(ss):
                for si in range(ss):
                    c = sample((i + (si + 0.5) / ss) / n, (j + (sj + 0.5) / ss) / n)
                    a = c[3]
                    acc[0] += c[0] * a; acc[1] += c[1] * a; acc[2] += c[2] * a; acc[3] += a
            a = acc[3] / (ss * ss)
            if acc[3]:
                row.append((int(acc[0] / acc[3]), int(acc[1] / acc[3]), int(acc[2] / acc[3]), int(a)))
            else:
                row.append((0, 0, 0, 0))
        px.append(row)
    # BMP (bottom-up) + AND mask
    data = struct.pack('<IiiHHIIiiII', 40, n, n * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    for row in reversed(px):
        for (r, g, b, a) in row:
            data += bytes((b, g, r, a))
    stride = ((n + 31) // 32) * 4
    for row in reversed(px):
        bits = bytearray(stride)
        for i, (_, _, _, a) in enumerate(row):
            if a == 0: bits[i // 8] |= 0x80 >> (i % 8)
        data += bytes(bits)
    return data

sizes = [16, 20, 24, 32, 40, 48]
imgs = [render(s) for s in sizes]
out = struct.pack('<HHH', 0, 1, len(sizes))
off = 6 + 16 * len(sizes)
for s, d in zip(sizes, imgs):
    out += struct.pack('<BBBBHHII', s, s, 0, 0, 1, 32, len(d), off); off += len(d)
for d in imgs: out += d
open(sys.argv[1], 'wb').write(out)
print('ok', len(out))
