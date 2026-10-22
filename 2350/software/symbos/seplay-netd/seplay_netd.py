#!/usr/bin/env python3
"""Patch SE Play 0.1 (SymbOS, SEPLAY.EXE) to use the SymbOS Network Daemon.

SE Play talks directly to TMTLogic's network card (TNC, I/O ports 40h-49h).
Without that card it hangs at start-up while reading the card's ID string.
This tool appends a small network layer (netd_seplay.asm) to the code
segment, relinks the executable using its SymbOS relocation table and
redirects the TNC routines to the new code, which uses the SymbOS Network
Daemon (e.g. the PicoVerse 2350 ESP-01 daemon netd-esp.exe).

usage: seplay_netd.py SEPLAY.EXE SEPLAYN.EXE
       seplay_netd.py --rebuild --sjasmplus PATH   (re-assemble netd_block.json)

Only Python 3 is needed to patch: the assembled block is stored in
netd_block.json. sjasmplus is only needed after changing netd_seplay.asm.
"""
import argparse, json, os, re, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
BLOCK_JSON = os.path.join(HERE, 'netd_block.json')
LOAD = 0x100                # SymbOS executables are linked at 0100h

# SE Play symbols (addresses as linked, before relinking)
CODE_SYMS = {               # inside the code segment: unchanged by the shift
    'STRASSIGN': 0x5ED7,    # string assign  HL=source chars, DE=dest data
    'SHOW1':     0x113A,    # refresh display line 1 (76BBh)
    'EMPTYSTR':  0x6135,    # "" constant
    'BANK':      0x010E,    # header: application ram bank (prgpstbnk)
}
DATA_SYMS = {               # data / transfer segment: shifted by the new block
    'V_HWID0':   0x75BB,    # string var: network card ID
    'V_IP':      0x7740,    # station IP, 4 words
    'V_PORT':    0x774A,    # station port, high byte (TNC order)
    'V_PORTLO':  0x774C,    # station port, low byte
    'V_RESULT':  0x7555,    # TNC routine result (0 ok / 1 error)
    'V_REQ':     0x75B9,    # string var: HTTP request
    'V_AVAIL':   0x75A9,    # bytes waiting
    'V_DISP1':   0x76BB,    # string var: display line 1
    'PID':       0x7E6E,    # own process ID (transfer area)
    'MSGBUF':    0x7E6F,    # SE Play message buffer (transfer area)
}

# (address, original bytes, replacement builder)
#   replacement: list of ('b', byte) or ('w', symbol | int) items
PATCHES = [
    (0x4FEC, 'f32a3575', [('b', 0xC3), ('w', 'net_find')]),     # TNC_GETID
    (0x50CF, '210000',   [('b', 0xC3), ('w', 'net_open')]),     # TNC_OPEN
    (0x518B, '210000',   [('b', 0xC3), ('w', 'net_send')]),     # TNC_SEND
    (0x5221, '210000',   [('b', 0xC3), ('w', 'net_close')]),    # TNC_CLOSE
    (0x5272, '210000',   [('b', 0xC3), ('w', 'net_rxavail')]),  # TNC_RXAVAIL
    (0x1D08, 'cd7252',   [('b', 0xC9)]),                         # TNC drain: nothing
    (0x1D7E, '2aa975ed5b3d75', [('b', 0xCD), ('w', 'net_pump'),  # copy loop
                                ('b', 0xC3), ('w', 0x1D94)]),
    (0x4E7D, '2a7d75',   [('b', 0xC9)]),                         # DBGPRINT to port 40h
    (0x2A45, 'cd2213',   [('b', 0xC3), ('w', 'phone_na')]),     # phone page
]


def rd16(b, o):
    return b[o] | (b[o + 1] << 8)


def assemble(sjasm, asm, org, shift, size_hint, workdir):
    """Assemble with ORG=org and all SE Play symbols moved by 'shift'."""
    inc = ['BLOCK_ORG equ 0x%04X' % org]
    for k, v in CODE_SYMS.items():
        inc.append('%s equ 0x%04X' % (k, v + shift))
    for k, v in DATA_SYMS.items():
        inc.append('%s equ 0x%04X' % (k, v + size_hint + shift))
    open(os.path.join(workdir, 'ext.inc'), 'w').write('\n'.join(inc) + '\n')
    out = os.path.join(workdir, 'blk.bin')
    sym = os.path.join(workdir, 'blk.sym')
    src = os.path.join(workdir, 'blk.asm')
    open(src, 'w').write(open(asm).read().replace('include "ext.inc"', 'include "%s"' % os.path.join(workdir, 'ext.inc')))
    r = subprocess.run([sjasm, '--nologo', '--msg=war', '--raw=' + out, '--sym=' + sym, src], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit('assembly failed:\n' + r.stdout + r.stderr)
    syms = {}
    for line in open(sym):
        m = re.match(r'(\w+):\s+EQU\s+0x([0-9A-Fa-f]+)', line)
        if m:
            syms[m.group(1)] = int(m.group(2), 16)
    return open(out, 'rb').read(), syms


def build_block(sjasm, asm, code_end):
    """Assemble the block twice (shifted by 1000h) to find its relocations."""
    with tempfile.TemporaryDirectory() as wd:
        blk0, _ = assemble(sjasm, asm, code_end, 0, 0, wd)
        n = (len(blk0) + 1) & ~1                       # keep data segment aligned
        blk, syms = assemble(sjasm, asm, code_end, 0, n, wd)
        blkb, _ = assemble(sjasm, asm, code_end + 0x1000, 0x1000, n, wd)
    if len(blk) != len(blkb) or len(blk) > n:
        sys.exit('block size changed between passes')
    blk = blk + bytes(n - len(blk))
    blkb = blkb + bytes(n - len(blkb))
    blk_rel = []
    i = 0
    while i < n - 1:
        wa, wb = rd16(blk, i), rd16(blkb, i)
        if wa != wb:
            if (wb - wa) & 0xFFFF != 0x1000:
                sys.exit('unexpected difference at block+%04X' % i)
            blk_rel.append(i)
            i += 2
        else:
            i += 1
    keep = ('net_find', 'net_open', 'net_send', 'net_close', 'net_rxavail', 'net_pump', 'phone_na')
    return {'org': code_end, 'size': n, 'code': blk.hex(), 'reloc': blk_rel,
            'syms': {k: syms[k] for k in keep}}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src', nargs='?')
    ap.add_argument('dst', nargs='?')
    ap.add_argument('--sjasmplus', help='assemble netd_seplay.asm instead of using netd_block.json')
    ap.add_argument('--asm', default=os.path.join(HERE, 'netd_seplay.asm'))
    ap.add_argument('--rebuild', action='store_true', help='re-assemble and update netd_block.json')
    a = ap.parse_args()

    if a.rebuild:
        if not a.sjasmplus:
            sys.exit('--rebuild needs --sjasmplus')
        blk = build_block(a.sjasmplus, a.asm, LOAD + 0x72E2)   # SE Play 0.1 code end
        json.dump(blk, open(BLOCK_JSON, 'w'), indent=1)
        print('wrote', BLOCK_JSON, '(%d bytes, %d relocations)' % (blk['size'], len(blk['reloc'])))
        return
    if not a.src or not a.dst:
        ap.error('need SEPLAY.EXE and output file')

    exe = bytearray(open(a.src, 'rb').read())
    if exe[0x30:0x38] != b'SymExe10':
        sys.exit('not a SymbOS executable')
    code, data, trans = struct.unpack_from('<3H', exe, 0)
    nrel = rd16(exe, 8)
    seg_end = code + data + trans                      # file offset of reloc table
    rel = [rd16(exe, seg_end + 2 * i) for i in range(nrel)]
    if len(exe) != seg_end + 2 * nrel:
        sys.exit('unexpected file layout')
    code_end = LOAD + code                              # first data address
    for addr, orig, _ in PATCHES:
        o = addr - LOAD
        if exe[o:o + len(orig) // 2].hex() != orig:
            sys.exit('this is not SE Play 0.1 (mismatch at %04Xh)' % addr)
    print('SE Play: code %04X data %04X transfer %04X, %d relocations' % (code, data, trans, nrel))

    if a.sjasmplus:
        b = build_block(a.sjasmplus, a.asm, code_end)
    else:
        b = json.load(open(BLOCK_JSON))
    if b['org'] != code_end:
        sys.exit('block was built for a different code size')
    n = b['size']
    blk = bytes.fromhex(b['code'])
    syms = b['syms']
    blk_rel = [code_end + r for r in b['reloc']]

    # rebuild image: code + block + data + transfer
    img = bytearray(exe[:code]) + blk + bytearray(exe[code:seg_end])

    def shift_addr(v):
        return v + n if v >= code_end else v

    new_rel = []
    for r in rel:
        r2 = shift_addr(r)
        o = r2 - LOAD
        v = rd16(img, o)
        v2 = shift_addr(v)
        img[o] = v2 & 0xFF
        img[o + 1] = v2 >> 8
        new_rel.append(r2)

    # patches in the original code (addresses below code_end are unchanged)
    for addr, orig, items in PATCHES:
        pos = addr
        out = bytearray()
        words = []
        for kind, val in items:
            if kind == 'b':
                out.append(val)
            else:
                v = syms[val] if isinstance(val, str) else val
                words.append(pos + len(out))
                out += struct.pack('<H', v)
        lo, hi = pos, pos + len(out)
        new_rel = [r for r in new_rel if not (r + 1 >= lo and r < hi)]   # drop overlapped
        img[pos - LOAD:pos - LOAD + len(out)] = out
        new_rel += words

    new_rel += blk_rel
    new_rel = sorted(set(new_rel))

    # header
    hdr = img
    struct.pack_into('<H', hdr, 0, code + n)
    struct.pack_into('<H', hdr, 8, len(new_rel))
    icon = rd16(hdr, 0x29)
    if hdr[0x28] & 1 and icon >= code:
        struct.pack_into('<H', hdr, 0x29, icon + n)
    out = bytes(img) + b''.join(struct.pack('<H', r) for r in new_rel)
    open(a.dst, 'wb').write(out)
    print('added %d bytes of code (%d relocations), %d relocations total' % (n, len(blk_rel), len(new_rel)))
    for k, v in syms.items():
        print('  %-12s %04Xh' % (k, v))
    print('wrote', a.dst, len(out), 'bytes')


if __name__ == '__main__':
    main()
