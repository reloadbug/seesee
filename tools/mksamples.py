import os, struct, zlib, colorsys
out = r"D:\AI game\seesee\tools\samples"
os.makedirs(out, exist_ok=True)
def png(path, w, h, rgb_fn):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        row = bytearray()
        for x in range(w):
            r,g,b = rgb_fn(x,y)
            row += bytes((r,g,b))
        raw += row
    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t+d) & 0xffffffff)
    ihdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)
    data = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) + chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b'')
    open(path,'wb').write(data)
for i in range(1, 61):
    hue = (i * 0.137) % 1.0
    r0,g0,b0 = [int(c*255) for c in colorsys.hsv_to_rgb(hue, 0.55, 0.85)]
    r1,g1,b1 = [int(c*255) for c in colorsys.hsv_to_rgb((hue+0.5)%1.0, 0.75, 0.35)]
    w, h = 480, 320
    def fn(x, y, r0=r0,g0=g0,b0=b0,r1=r1,g1=g1,b1=b1,w=w,h=h):
        t = (x/w + y/h) / 2
        return (int(r0+(r1-r0)*t), int(g0+(g1-g0)*t), int(b0+(b1-b0)*t))
    png(os.path.join(out, f"sample_{i:02d}.png"), w, h, fn)
print("ok", len(os.listdir(out)))
