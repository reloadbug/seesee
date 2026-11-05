#!/usr/bin/env python3
"""生成 seesee.ico —— 纯标准库，无需 Pillow。

图形：深色圆角方块 + 强调色圆环 + 中心瞳孔点（与 UI 里的品牌点一致）
"""
import os
import struct

ACCENT = (0x5B, 0x9C, 0xFF)
BG = (0x1B, 0x1E, 0x23)
BG2 = (0x12, 0x15, 0x1A)


def render(size: int) -> list:
    """返回 size*size 的 (r,g,b,a) 列表，4x4 超采样抗锯齿。"""
    SS = 4
    W = size * SS
    canvas = [[(0, 0, 0, 0)] * W for _ in range(W)]

    # 圆角半径与圆环参数（归一化）
    r_corner = 0.22
    ring_r = 0.335
    ring_w = 0.075
    pupil_r = 0.105

    inv = 1.0 / W
    for y in range(W):
        ny = (y + 0.5) * inv - 0.5
        for x in range(W):
            nx = (x + 0.5) * inv - 0.5
            # 圆角方块（超椭圆近似）
            ax, ay = abs(nx) * 2.0, abs(ny) * 2.0
            k = 0.0
            if ax > 1 - r_corner * 2 and ay > 1 - r_corner * 2:
                cx = (1 - r_corner * 2)
                dx, dy = ax - cx, ay - cx
                rr = r_corner * 2
                d = (dx * dx + dy * dy) ** 0.5
                k = 1.0 - min(1.0, d / rr) if d < rr else 0.0
                if d >= rr:
                    continue
            # 背景垂直渐变
            t = (ny + 0.5)
            bg = tuple(int(BG[i] + (BG2[i] - BG[i]) * t) for i in range(3))
            col = (bg[0], bg[1], bg[2], 255)

            d = (nx * nx + ny * ny) ** 0.5
            if abs(d - ring_r) < ring_w * 0.5:
                col = (ACCENT[0], ACCENT[1], ACCENT[2], 255)
            elif d < pupil_r:
                col = (ACCENT[0], ACCENT[1], ACCENT[2], 255)
            canvas[y][x] = col

    # 下采样
    out = []
    area = SS * SS
    for y in range(size):
        for x in range(size):
            r = g = b = a = 0
            for dy in range(SS):
                for dx in range(SS):
                    c = canvas[y * SS + dy][x * SS + dx]
                    r += c[0] * c[3]
                    g += c[1] * c[3]
                    b += c[2] * c[3]
                    a += c[3]
            if a == 0:
                out.append((0, 0, 0, 0))
            else:
                out.append((r // a, g // a, b // a, a // area))
    return out


def to_ico_image(size: int) -> bytes:
    px = render(size)
    hdr = struct.pack('<IiiHHIIiiII', 40, size, size * 2, 1, 32, 0, size * size * 4, 0, 0, 0, 0)
    body = bytearray()
    for y in range(size - 1, -1, -1):  # BMP 自下而上
        for x in range(size):
            r, g, b, a = px[y * size + x]
            body += bytes((b, g, r, a))
    mask_row = ((size + 31) // 32) * 4
    mask = b'\x00' * (mask_row * size)
    return hdr + bytes(body) + mask


def main():
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    images = [to_ico_image(s) for s in sizes]
    out = bytearray(struct.pack('<HHH', 0, 1, len(sizes)))
    offset = 6 + 16 * len(sizes)
    for s, img in zip(sizes, images):
        w = 0 if s >= 256 else s
        out += struct.pack('<BBBBHHII', w, w, 0, 0, 1, 32, len(img), offset)
        offset += len(img)
    for img in images:
        out += img

    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, '..', 'resources', 'seesee.ico')
    with open(path, 'wb') as f:
        f.write(bytes(out))
    print('wrote', os.path.abspath(path), len(out), 'bytes')


if __name__ == '__main__':
    main()
