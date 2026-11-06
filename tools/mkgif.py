# 生成测试用动画 GIF（纯标准库，不依赖 Pillow）
# 用法: python tools/mkgif.py [输出路径]
import struct
import sys


def lzw_literal(indices, min_code_size=8):
    """只发字面量的 LZW：固定 9 位码，每 250 个像素插一个 clear，避免码表溢出。"""
    clear = 1 << min_code_size
    eoi = clear + 1
    code_size = min_code_size + 1
    out = bytearray()
    bitbuf = 0
    bitcnt = 0

    def emit(code):
        nonlocal bitbuf, bitcnt
        bitbuf |= code << bitcnt
        bitcnt += code_size
        while bitcnt >= 8:
            out.append(bitbuf & 0xFF)
            bitbuf >>= 8
            bitcnt -= 8

    emit(clear)
    n = 0
    for px in indices:
        emit(px)
        n += 1
        if n == 250:
            emit(clear)
            n = 0
    emit(eoi)
    if bitcnt:
        out.append(bitbuf & 0xFF)
    return bytes(out)


def sub_blocks(data):
    out = bytearray()
    for i in range(0, len(data), 255):
        chunk = data[i:i + 255]
        out.append(len(chunk))
        out += chunk
    out.append(0)
    return bytes(out)


def make_gif(path, w=320, h=240, frames=8, delay_cs=8):
    # 调色板：黑 + 一条彩虹渐变
    palette = bytearray()
    palette += bytes([0, 0, 0])
    for i in range(255):
        t = i / 254.0
        r = int(255 * (0.5 + 0.5 * (1 - abs(t * 2 - 1))))
        g = int(255 * (0.2 + 0.8 * t))
        b = int(255 * (0.9 - 0.7 * t))
        palette += bytes([r, g, b])
    assert len(palette) == 768

    out = bytearray()
    out += b"GIF89a"
    out += struct.pack("<HH", w, h)
    out += bytes([0xF7, 0, 0])  # GCT 256 色, 背景 0, 无长宽比
    out += palette
    # NETSCAPE 循环
    out += b"\x21\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00"

    for f in range(frames):
        # 帧控制：disposal = 1（不处置）
        out += b"\x21\xF9\x04" + bytes([1 << 2]) + struct.pack("<H", delay_cs) + b"\x00\x00"
        out += b"\x2C" + struct.pack("<HHHH", 0, 0, w, h) + b"\x00"
        indices = []
        bar = (f * w // frames)
        for y in range(h):
            for x in range(w):
                if abs(x - bar) < 18:
                    indices.append(255)  # 亮条
                else:
                    v = (x * 255 // w + y * 255 // h + f * 20) % 255
                    indices.append(1 + v)
        data = lzw_literal(indices)
        out += bytes([8]) + sub_blocks(data)

    out += b"\x3B"
    with open(path, "wb") as fh:
        fh.write(out)
    return len(out)


if __name__ == "__main__":
    dst = sys.argv[1] if len(sys.argv) > 1 else "tools/samples/anim_test.gif"
    n = make_gif(dst)
    print("wrote %s (%d bytes)" % (dst, n))
