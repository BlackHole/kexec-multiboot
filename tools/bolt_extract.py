#!/usr/bin/env python3
"""
bolt_extract.py - extract the display setup that a Vu+ 4K BOLT image applies
for its boot splash, into a neutral JSON file (no vendor headers needed).

usage: bolt_extract.py <bolt.bin> > <box>.vufbmode.json

What the BOLT carries (found by structure, not by symbol names):
  regs      list of (register offset, value) written once, in order
  displays  per display: surface index, video format code, and the one or
            two "scratch" registers the display lists read the surface
            address from
  lists     per register-DMA slot: trigger number, slot, and a chain of
            command lists (count table + words) re-run on every vsync
Register offsets are relative to the 0xF0000000 register window; the
0x20000000 bus prefix some chips use is removed from `regs` only.
"""
import json, struct, sys

data = open(sys.argv[1], 'rb').read()
n = len(data) // 4
w = struct.unpack('<%dI' % n, data[:n * 4])
# BOLT data pointers: 0x06xxxxxx-0x07xxxxxx (32-bit BOLT), 0x4xxxxxxx (64-bit BOLT, 7278)
is_ptr = lambda x: 0x06000000 <= x < 0x08000000 or 0x40000000 <= x < 0x50000000


def find_slot_table():
    """runs of {trigger<64, slot<64, maxidx<64, ptr, ptr}"""
    for i in range(n - 10):
        if all(w[i+5*k] < 64 and w[i+5*k+1] < 64 and w[i+5*k+2] < 64 and
               is_ptr(w[i+5*k+3]) and is_ptr(w[i+5*k+4]) for k in range(2)):
            ents, k = [], 0
            while w[i+5*k] < 64 and is_ptr(w[i+5*k+3]):
                ents.append(w[i+5*k:i+5*k+5])
                k += 1
            return i, ents
    sys.exit('slot table not found')


tab, slots = find_slot_table()

# load address: the count tables start at 0 and grow
delta = None
p0 = slots[0][3]
for dl in range(-(p0 & ~0xfff), -(p0 & ~0xfff) + len(data), 0x1000):
    ok = True
    for t, s, m, pc, pl in slots:
        o = (pc + dl) // 4
        if not 0 <= o < n - m - 1:
            ok = False
            break
        c = w[o:o + m + 1]
        if not (c[0] == 0 and all(c[j] < c[j + 1] for j in range(m))):
            ok = False
            break
    if ok:
        delta = dl
        break
if delta is None:
    sys.exit('load address not found')

# display entries {surface, format, scratch0, scratch1} just before the
# slot table (unused slot entries, all 0xffffffff, sit in between)
j = tab
while w[j-5] == 0xffffffff and w[j-4] == 0xffffffff:
    j -= 5
disp = []
while True:
    e = w[j-4:j]
    if e[0] < 4 and e[1] < 0x100 and (e[2] & 0x0fff0000) == 0x00600000:
        disp.insert(0, e)
        j -= 4
    else:
        break
# 64-bit BOLT (7278): display entries elsewhere, {surface, format, scratch, 0}
# with scratch offsets below 0x10000 (register window at 0x08000000)
if not disp:
    for k in range(n - 8):
        if (w[k] == 0 and w[k+1] < 0x100 and 0x4000 <= w[k+2] < 0x10000 and w[k+3] == 0 and
                w[k+4] == 1 and w[k+5] < 0x100 and 0x4000 <= w[k+6] < 0x10000 and w[k+7] == 0):
            disp = [w[k:k+4], w[k+4:k+8]]
            break

# register list: longest run of (offset, value) pairs
is_reg = lambda x: (x >> 24) in (0x00, 0x20) and x % 4 == 0 and 0x10000 <= (x & 0x0fffffff) < 0x2000000
best = (0, 0)
for par in (0, 1):
    q = par
    while q < n - 2:
        e = q
        while e < n and is_reg(w[e]):
            e += 2
        if e - q > best[1] - best[0]:
            best = (q, e)
        q = e + 2 if e > q else q + 2
r0, r1 = best
norm = lambda a: a & 0x0fffffff if (a >> 24) == 0x20 else a
regs = [[norm(a), v] for a, v in zip(w[r0:r1:2], w[r0+1:r1:2])]

lists = []
for t, s, m, pc, pl in slots:
    c = list(w[(pc + delta)//4:(pc + delta)//4 + m + 1])
    l = list(w[(pl + delta)//4:(pl + delta)//4 + c[-1]])
    lists.append({'trigger': t, 'slot': s, 'counts': c, 'words': l})

json.dump({
    'source': sys.argv[1].replace('\\', '/').split('/')[-1],
    'regs': regs,
    'displays': [{'surface': e[0], 'format': e[1],
                  'scratch': [norm(e[2]), norm(e[3])]} for e in disp],
    'lists': lists,
}, sys.stdout, separators=(',', ':'))
