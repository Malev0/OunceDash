# Dev helper: summarise an Android arm64 .so's segments and relocations.
import struct, sys, collections
d = open(sys.argv[1], 'rb').read()
e_phoff, = struct.unpack_from('<Q', d, 0x20)
e_phnum, = struct.unpack_from('<H', d, 0x38)
phs = [struct.unpack_from('<IIQQQQQQ', d, e_phoff + i*56) for i in range(e_phnum)]
loads = [p for p in phs if p[0] == 1]
dyn = [p for p in phs if p[0] == 2][0]
tags = {}
off = dyn[2]
while True:
    t, v = struct.unpack_from('<qQ', d, off); off += 16
    if t == 0: break
    tags.setdefault(t, v)
rw = [p for p in loads if p[1] & 2][0]
rw_start = rw[3] & ~0xfff
types = collections.Counter(); bad = 0
for (a, sz) in ((tags.get(7), tags.get(8)), (tags.get(23), tags.get(2))):
    if not a: continue
    for i in range(sz // 24):
        r_off, r_info, r_add = struct.unpack_from('<QQq', d, a + i*24)
        types[r_info & 0xffffffff] += 1
        if r_off < rw_start: bad += 1
print('loads', [(hex(p[3]), hex(p[5]), hex(p[6]), p[1]) for p in loads])
print('reloc types', dict(types), 'below RW:', bad)
