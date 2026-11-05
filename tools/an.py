import struct, zlib, sys
p = r"D:\AI game\seesee\shot.png"
data = open(p,'rb').read()
# minimal PNG decode (we wrote 32bpp BGRA, no filters other than default)
w,h = struct.unpack('>II', data[16:24])
bitdepth, colortype = data[24], data[25]
print("size", w, h, "bitdepth", bitdepth, "colortype", colortype)
# collect IDAT
idat = b''
i = 8
while i < len(data):
    ln = struct.unpack('>I', data[i:i+4])[0]
    typ = data[i+4:i+8]
    if typ == b'IDAT': idat += data[i+8:i+8+ln]
    i += 12 + ln
raw = zlib.decompress(idat)
bpp = 4
stride = w*bpp
rows = []
prev = bytearray(stride)
pos = 0
for y in range(h):
    f = raw[pos]; pos += 1
    line = bytearray(raw[pos:pos+stride]); pos += stride
    if f == 1:
        for x in range(bpp, stride): line[x] = (line[x] + line[x-bpp]) & 0xff
    elif f == 2:
        for x in range(stride): line[x] = (line[x] + prev[x]) & 0xff
    elif f == 3:
        for x in range(stride):
            a = line[x-bpp] if x >= bpp else 0
            line[x] = (line[x] + ((a + prev[x]) >> 1)) & 0xff
    elif f == 4:
        for x in range(stride):
            a = line[x-bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x-bpp] if x >= bpp else 0
            pp = a + b - c
            pa, pb, pc = abs(pp-a), abs(pp-b), abs(pp-c)
            pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
            line[x] = (line[x] + pr) & 0xff
    rows.append(line)
    prev = line
# find titlebar bottom: scan column x=5 from top for color change from 0x16191E
def px(x,y):
    r = rows[y]; o = x*4
    return (r[o+2], r[o+1], r[o])  # RGB
print("px(5,5)", px(5,5), "px(5,40)", px(5,40), "px(5,100)", px(5,100))
# detect rows where left edge is not titlebar color
titlecol = px(5,5)
y = 0
while y < h and px(5,y) == titlecol: y += 1
print("titlebar height ~", y)
# filmstrip: scan from bottom at x=5
botcol = px(5,h-5)
print("bottom color", botcol)
y2 = h-1
while y2 > 0 and px(5,y2) == botcol: y2 -= 1
print("filmstrip height ~", h-1-y2)
