import sys, struct, zlib
def conv(src, dst):
    d = open(src,'rb').read()
    off = struct.unpack_from('<I', d, 10)[0]; W, H = struct.unpack_from('<ii', d, 18)
    row = (W*3+3)&~3
    out = bytearray()
    for y in range(H-1, -1, -1):
        out.append(0)
        line = d[off+y*row: off+y*row+W*3]
        for x in range(W): out += bytes((line[x*3+2], line[x*3+1], line[x*3]))
    def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t+b) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(bytes(out), 6)) + chunk(b'IEND', b'')
    open(dst,'wb').write(png)
for a in sys.argv[1:]: conv(a, a[:-4]+'.png')
