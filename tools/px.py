import zlib, struct, sys
p = sys.argv[1]
d = open(p,'rb').read()
assert d[:8] == b'\x89PNG\r\n\x1a\n'
pos = 8; idat = b''; w=h=0; bd=ct=0
while pos < len(d):
    ln = struct.unpack('>I', d[pos:pos+4])[0]; typ = d[pos+4:pos+8]; data = d[pos+8:pos+8+ln]; pos += 12+ln
    if typ == b'IHDR':
        w,h,bd,ct = struct.unpack('>IIBB', data[:10])
    elif typ == b'IDAT':
        idat += data
    elif typ == b'IEND':
        break
raw = zlib.decompress(idat)
bpp = 4 if ct == 6 else 3
stride = w*bpp
rows = []
prev = bytearray(stride)
i = 0
for y in range(h):
    f = raw[i]; i += 1
    line = bytearray(raw[i:i+stride]); i += stride
    if f == 1:
        for x in range(bpp, stride): line[x] = (line[x] + line[x-bpp]) & 255
    elif f == 2:
        for x in range(stride): line[x] = (line[x] + prev[x]) & 255
    elif f == 3:
        for x in range(stride):
            a = line[x-bpp] if x >= bpp else 0
            line[x] = (line[x] + ((a + prev[x]) >> 1)) & 255
    elif f == 4:
        for x in range(stride):
            a = line[x-bpp] if x >= bpp else 0
            b = prev[x]; c = prev[x-bpp] if x >= bpp else 0
            pp = a + b - c
            pa, pb, pc = abs(pp-a), abs(pp-b), abs(pp-c)
            pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
            line[x] = (line[x] + pr) & 255
    rows.append(bytes(line)); prev = line
y = int(sys.argv[2])
row = rows[y]
out = []
for x in range(int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])):
    o = x*bpp
    out.append("%d:(%d,%d,%d)" % (x, row[o], row[o+1], row[o+2]))
print(" ".join(out))
