"""
MSX PICOVERSE PROJECT
(c) 2026 Cristiano Goncalves
The Retro Hacker

z80_mailbox_test.py - Test of the Z80 code of the USB FDD BIOS against the PicoVerse mailbox

Runs the routines of the real BIOS ROM (msx/src, assembled with sjasm) in a Z80 emulator
(pip install z80). The mailbox registers (7D00h-7FCFh) are emulated here with the same behavior as
the RP2040 firmware (msxusbfdd.c), and the file commands are executed by the real fs_service.c and
FatFs, built as a shared library (pv_host_lib.c). The USB floppy drive is a small Python model.

Run it with "make z80test" in this directory.
"""

import ctypes
import os
import re
import shutil
import subprocess
import sys
import tempfile

from z80 import Z80Machine

HERE = os.path.dirname(os.path.abspath(__file__))
MSX_SRC = os.path.join(HERE, "..", "msx", "src")
SJASM = os.path.join(HERE, "..", "msx", "tools", "sjasm.exe")
if not os.path.exists(SJASM):
    SJASM = os.path.join(HERE, "..", "msx", "tools", "sjasm")
LIB = os.path.join(HERE, "build", "pvhost.dll" if os.name == "nt" else "libpvhost.so")

# Mailbox
PV_WIN, PV_REGS, PV_END = 0x7D00, 0x7E00, 0x7FCF
CMD_RESET, CMD_DEV_CHANGE, CMD_DEV_INFO, CMD_BUS_RESET, CMD_LONG_TIMEOUT, CMD_UFI_EXEC = 1, 2, 3, 4, 5, 0x10
CMD_ABORT = 0xFF
DEV_NONE, DEV_FDD, DEV_STORAGE, DEV_OTHER = 0, 1, 2, 3
RES_OK, RES_NO_DEVICE, RES_PANIC, RES_DATA_ERROR = 0, 5, 6, 4

failures = 0
checks = 0


def check(cond, msg):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print("  FAIL: %s" % msg)


def build_rom():
    """Assemble the BIOS with a listing to get the addresses of the routines."""
    tmp = tempfile.mkdtemp()
    version = os.path.join(HERE, "..", "msx", "build")
    os.makedirs(version, exist_ok=True)
    vfile = os.path.join(version, "version.asm")
    if not os.path.exists(vfile):
        with open(vfile, "w") as f:
            f.write('    db "test"\n')
    rom = os.path.join(tmp, "test.rom")
    subprocess.run([SJASM, "-l", "msxusbfdd.asm", rom], cwd=MSX_SRC, check=True, stdout=subprocess.DEVNULL)
    lst = os.path.join(MSX_SRC, "msxusbfdd.lst")
    syms = {}
    for line in open(lst, errors="replace"):
        m = re.match(r"^\s*\d+\+*\s+([0-9A-F]{4})\s+([A-Za-z_][A-Za-z0-9_]*):", line)
        if m:
            syms[m.group(2)] = int(m.group(1), 16)
    os.remove(lst)
    data = open(rom, "rb").read()
    shutil.rmtree(tmp)
    return data, syms


class FakeFloppy:
    """A 1.44MB USB floppy drive model behind the PV_CMD_UFI_EXEC command."""

    def __init__(self):
        self.sectors = bytearray(2880 * 512)
        for i in range(len(self.sectors)):
            self.sectors[i] = (i * 7 + (i >> 9)) & 0xFF
        self.fail_next = None       # (asc, ascq) returned by the next command
        self.commands = []

    def execute(self, cdb, length, out, data):
        """Returns (usb_result, data_in, asc, ascq)."""
        self.commands.append((bytes(cdb), length, out))
        if self.fail_next:
            asc, ascq = self.fail_next
            self.fail_next = None
            return RES_OK, b"", asc, ascq
        op = cdb[0]
        if op == 0x28:  # READ(10)
            lba = int.from_bytes(cdb[2:6], "big")
            count = int.from_bytes(cdb[7:9], "big")
            return RES_OK, bytes(self.sectors[lba * 512:(lba + count) * 512]), 0, 0
        if op == 0x2A:  # WRITE(10)
            lba = int.from_bytes(cdb[2:6], "big")
            count = int.from_bytes(cdb[7:9], "big")
            self.sectors[lba * 512:(lba + count) * 512] = data[:count * 512]
            return RES_OK, b"", 0, 0
        if op == 0x23:  # READ FORMAT CAPACITIES: one 1.44MB descriptor
            return RES_OK, (bytes([0, 0, 0, 0x20, 0, 0, 0x0B, 0x40, 2, 0, 2, 0]) + bytes(24))[:length], 0, 0
        if op == 0x25:  # READ CAPACITY: last LBA 2879, block size 512
            return RES_OK, bytes([0, 0, 0x0B, 0x3F, 0, 0, 2, 0])[:length], 0, 0
        if op == 0x12:  # INQUIRY
            inq = bytearray(36)
            inq[8:16] = b"TEAC    "
            inq[16:32] = b"FD-05PUB       "
            inq[32:36] = b"1026"
            return RES_OK, bytes(inq[:length]), 0, 0
        return RES_OK, b"", 0, 0


class Mailbox:
    """The mailbox registers as the firmware shows them to the MSX."""

    def __init__(self, lib):
        self.lib = lib
        lib.pv_host_mbox.restype = ctypes.c_void_p
        lib.pv_host_command.restype = ctypes.c_uint8
        lib.pv_host_command.argtypes = [ctypes.c_uint8]
        base = lib.pv_host_mbox()
        self.o_status = lib.pv_host_offset(0)
        self.o_result = lib.pv_host_offset(1)
        self.o_page = lib.pv_host_offset(2)
        self.o_arg = lib.pv_host_offset(3)
        self.o_resp = lib.pv_host_offset(4)
        self.o_buf = lib.pv_host_offset(5)
        self.mem = (ctypes.c_uint8 * (self.o_buf + 8192)).from_address(base)
        self.pending = 0
        self.delay = 3          # status reads that return BUSY before a command completes
        self.abort = False
        self.dev_type = DEV_NONE
        self.hub = 0
        self.diag = [0] * 10    # floppy transport diagnostics (RESP6-15 of DEV_INFO)
        self.events = 0         # 1 connect, 0xFF disconnect
        self.floppy = FakeFloppy()
        self.commands = []

    # --- registers
    def read(self, addr):
        if PV_WIN <= addr < PV_REGS:
            return self.mem[self.o_buf + ((self.mem[self.o_page] & 0x1F) << 8) + (addr & 0xFF)]
        reg = addr - PV_REGS
        if reg == 0:
            return self.read_status()
        if reg == 1:
            return self.mem[self.o_result]
        if reg == 2:
            return self.mem[self.o_page]
        if 4 <= reg < 20:
            return self.mem[self.o_arg + reg - 4]
        if 20 <= reg < 36:
            return self.mem[self.o_resp + reg - 20]
        if 0x30 <= reg < 0x34:
            return b"PVFD"[reg - 0x30]
        if reg == 0x34:
            return 1
        return 0xFF

    def write(self, addr, value):
        if PV_WIN <= addr < PV_REGS:
            self.mem[self.o_buf + ((self.mem[self.o_page] & 0x1F) << 8) + (addr & 0xFF)] = value
            return
        reg = addr - PV_REGS
        if reg == 2:
            self.mem[self.o_page] = value & 0x1F
        elif reg == 3:
            self.command(value)
        elif 4 <= reg < 20:
            self.mem[self.o_arg + reg - 4] = value

    def command(self, cmd):
        if cmd == CMD_ABORT:
            if self.pending:
                self.abort = True
            return
        if cmd == 0 or self.pending:
            return
        self.commands.append(cmd)
        self.abort = False
        self.mem[self.o_result] = 0
        self.mem[self.o_status] = 0x80
        self.pending = cmd
        self.countdown = self.delay

    def read_status(self):
        if self.pending:
            if self.abort:
                self.finish(RES_PANIC)
            elif self.countdown > 0:
                self.countdown -= 1
            else:
                self.run_pending()
        return self.mem[self.o_status]

    def finish(self, result):
        self.mem[self.o_result] = result
        self.pending = 0
        self.mem[self.o_status] = 0

    def arg(self, i):
        return self.mem[self.o_arg + i]

    def set_resp(self, i, v):
        self.mem[self.o_resp + i] = v

    def run_pending(self):
        cmd = self.pending
        for i in range(16):
            self.mem[self.o_resp + i] = 0
        if 0x20 <= cmd <= 0x2B:
            self.finish(self.lib.pv_host_command(cmd))
        elif cmd == CMD_RESET:
            self.events = 0
            self.set_resp(0, 1 if self.dev_type != DEV_NONE else 0xFF)
            self.finish(RES_OK)
        elif cmd == CMD_DEV_CHANGE:
            self.set_resp(0, self.events)
            self.events = 0
            self.finish(RES_OK)
        elif cmd == CMD_DEV_INFO:
            self.set_resp(0, self.dev_type)
            self.set_resp(5, self.hub)
            for i, v in enumerate(self.diag):
                self.set_resp(6 + i, v)
            self.finish(RES_OK)
        elif cmd in (CMD_BUS_RESET, CMD_LONG_TIMEOUT):
            self.finish(RES_OK)
        elif cmd == CMD_UFI_EXEC:
            self.do_ufi()
        else:
            self.finish(7)

    def do_ufi(self):
        cdb = bytes(self.mem[self.o_arg + i] for i in range(12))
        length = self.arg(12) | (self.arg(13) << 8)
        out = self.arg(14) & 1
        data = bytes(self.mem[self.o_buf + i] for i in range(length)) if out else b""
        if self.dev_type != DEV_FDD:
            self.finish(RES_NO_DEVICE)
            return
        res, data_in, asc, ascq = self.floppy.execute(cdb, length, out, data)
        for i, b in enumerate(data_in):
            self.mem[self.o_buf + i] = b
        n = len(data_in) if not out else length
        self.set_resp(0, n & 0xFF)
        self.set_resp(1, n >> 8)
        self.set_resp(2, asc)
        self.set_resp(3, ascq)
        self.finish(res)


class Msx:
    """Z80 emulator with the BIOS ROM mapped with the Konami SCC mapper and the mailbox."""

    RET = 0xE000

    def __init__(self, rom, syms, mailbox):
        self.rom = rom
        self.sym = syms
        self.mb = mailbox
        self.banks = [2, 3]     # ROM bank 1 (the driver) is the one mapped when its routines are called
        self.keys_pressed = False
        self.m = Z80Machine()
        m = self.m
        m.set_read_callback(self.on_read)
        m.set_write_callback(self.on_write)
        m.set_input_callback(self.on_input)
        m.set_output_callback(self.on_output)
        m.mark_addrs(0x4000, 0x4000, Z80Machine.READ_MARK | Z80Machine.WRITE_MARK)
        # RDPRIM routine of the BIOS (RAM hook at F365h): XOR A / RET
        m.memory[0xF365:0xF368] = bytes([0xAF, 0xC9, 0xC9])
        m.memory[self.RET] = 0x76
        m.set_breakpoint(self.RET)
        # CHPUT (BIOS 00A2h): OUT (0FEh),A / RET, the output is collected in self.printed
        m.memory[0x00A2:0x00A5] = bytes([0xD3, 0xFE, 0xC9])
        self.printed = bytearray()
        self.ppi_c = 0

    def on_read(self, addr):
        if PV_WIN <= addr <= PV_END:
            return self.mb.read(addr)
        if 0x4000 <= addr < 0x8000:
            bank = self.banks[0 if addr < 0x6000 else 1]
            return self.rom[(bank << 13) | (addr & 0x1FFF)]
        return 0xFF

    def on_write(self, addr, value):
        if PV_WIN <= addr <= PV_END:
            self.mb.write(addr, value)
        elif (addr & 0xF800) == 0x5000:
            self.banks[0] = value & 3
        elif (addr & 0xF800) == 0x7000:
            self.banks[1] = value & 3

    def on_output(self, port, value):
        if (port & 0xFF) == 0xFE:
            self.printed.append(value)
        elif (port & 0xFF) == 0xAA:
            self.ppi_c = value

    def on_input(self, port):
        if (port & 0xFF) == 0xA9:
            row = self.ppi_c & 0x0F
            value = 0xFF
            if self.keys_pressed:
                if row == 6:
                    value &= ~0x08      # CAPS
                if row == 7:
                    value &= ~0x04      # ESC
            return value
        return 0xFF

    def call(self, name, max_instr=50_000_000, **regs):
        m = self.m
        m.sp = 0xEF00
        m.memory[0xEEFE] = self.RET & 0xFF
        m.memory[0xEEFF] = self.RET >> 8
        m.sp = 0xEEFE
        m.pc = self.sym[name] if isinstance(name, str) else name
        for k, v in regs.items():
            setattr(m, k, v)
        m.f = regs.get("f", 0)
        left = max_instr
        while True:
            m.ticks_to_stop = 20_000_000
            m.run()
            if m.pc == self.RET:
                break
            left -= 1
            if left <= 0:
                raise RuntimeError("routine %s did not return" % name)
        return m

    # --- memory helpers
    def put(self, addr, data):
        self.m.memory[addr:addr + len(data)] = data

    def get(self, addr, n):
        return bytes(self.m.memory[addr:addr + n])

    def cstr(self, addr, s):
        self.put(addr, s.encode() + b"\0")


def carry(m):
    return bool(m.f & 1)


def zero(m):
    return bool(m.f & 0x40)


def main():
    lib = ctypes.CDLL(LIB)
    lib.pv_host_init.argtypes = [ctypes.c_uint32, ctypes.c_int]
    mb = Mailbox(lib)
    rom, syms = build_rom()
    msx = Msx(rom, syms, mb)
    m = msx.m
    S = 0x9000      # strings
    B = 0xA000      # buffers

    print("== HW_TEST / HW_RESET")
    msx.call("HW_TEST")
    check(not carry(m), "HW_TEST succeeds with the mailbox present")
    mb.dev_type = DEV_NONE
    msx.call("HW_RESET")
    check(not carry(m) and m.a == 0xFF, "HW_RESET reports no device (A=%02X)" % m.a)
    mb.dev_type = DEV_STORAGE
    msx.call("HW_RESET")
    check(not carry(m) and m.a == 1, "HW_RESET reports a device (A=%02X)" % m.a)
    mb.events = 1
    msx.call("HW_DEV_CHANGE")
    check(not carry(m) and m.a == 1, "HW_DEV_CHANGE reports the connection")
    msx.call("HW_DEV_CHANGE")
    check(not carry(m) and m.a == 0, "HW_DEV_CHANGE reports no change afterwards")
    mb.events = 0xFF
    msx.call("HW_DEV_CHANGE")
    check(not carry(m) and m.a == 0xFF, "HW_DEV_CHANGE reports the disconnection")

    for label, sectors, fat32 in (("FAT16", 131072, 0), ("FAT32", 262144, 1)):
        print("== storage device (%s)" % label)
        check(lib.pv_host_init(sectors, fat32) == 0, "RAM disk created")
        mb.dev_type = DEV_STORAGE

        msx.call("HWF_MOUNT_DISK", hl=B)
        check(not carry(m), "HWF_MOUNT_DISK succeeds")
        check(msx.get(B, 28) == b"TestVndrTest Drive      0.01" and msx.get(B + 28, 1) == b"\0",
              "device name copied: %r" % msx.get(B, 29))

        msx.cstr(S, "GAMES/ONE.DSK")
        msx.call("HWF_OPEN_FILE_DIR", hl=S)
        check(carry(m) and zero(m) and m.a == 0, "open GAMES: directory (A=%d F=%02X)" % (m.a, m.f))
        check(m.hl == S + 5, "HL points to the '/' terminator (HL=%04X)" % m.hl)
        msx.cstr(S + 32, "ONE.DSK")
        msx.call("HWF_OPEN_FILE_DIR", hl=S + 32)
        check(not carry(m) and zero(m) and m.a == 0, "open ONE.DSK: file")
        check(m.hl == S + 32 + 7, "HL points to the zero terminator")
        msx.cstr(S, "NOFILE.DSK")
        msx.call("HWF_OPEN_FILE_DIR", hl=S)
        check(not zero(m) and m.a == 2, "missing file gives A=2 (A=%d)" % m.a)
        msx.cstr(S, "/")
        msx.call("HWF_OPEN_FILE_DIR", hl=S)
        check(carry(m) and zero(m), "open / gives the root directory")

        long_name = "A" * 100 + "/REST"
        msx.cstr(S, long_name)
        msx.call("HWF_OPEN_FILE_DIR", hl=S)
        check(not zero(m) and m.a in (1, 2) and m.hl == S + 100, "a name longer than the buffer is truncated, HL=%04X A=%d" % (m.hl, m.a))

        # --- enumeration
        msx.call("HWF_ENUM_FILES", hl=B, bc=100)
        count = m.bc
        entries = [msx.get(B + 11 * i, 11) for i in range(count)]
        check(count == 3, "3 visible entries, found %d: %r" % (count, entries))
        check(m.hl == B + 33 and msx.get(m.hl, 1) == b"\0", "list ends with a zero byte")
        names = {e[:10] for e in entries}
        check(b"DISK1   DS" in names and b"README  TX" in names and b"GAMES     " in names,
              "names: %r" % entries)
        games = [e for e in entries if e.startswith(b"GAMES")]
        check(games and games[0][10] & 0x80, "the directory has bit 7 set in the last character")

        msx.call("HWF_ENUM_FILES", hl=B, bc=1)
        check(m.bc == 1, "enumeration limited to 1 entry")

        for n, expected in ((0, b"DISK1   DSK"), (1, b"README  TXT")):
            msx.call("HWF_FIND_NTH_FILE", hl=B, a=n)
            check(m.a == 0 and msx.get(B, 11) == expected, "find file %d: A=%d %r" % (n, m.a, msx.get(B, 11)))
        msx.call("HWF_FIND_NTH_FILE", hl=B, a=2)
        check(m.a == 2, "find file 2 (only a directory left) gives not found (A=%d)" % m.a)

        # --- read the 720K image
        msx.cstr(S, "DISK1.DSK")
        msx.call("HWF_OPEN_FILE_DIR", hl=S)
        check(not carry(m) and zero(m), "open DISK1.DSK")
        image = bytes(((i * 31 + (i >> 8)) & 0xFF) for i in range(720 * 1024))
        ok = True
        for sector in (0, 1, 17, 359, 700, 1439):
            msx.call("HWF_SEEK_FILE", hl=(sector * 512) >> 16, de=(sector * 512) & 0xFFFF)
            ok &= m.a == 0
            msx.call("HWF_READ_FILE", hl=0xC000, bc=512)
            ok &= m.a == 0 and m.bc == 512 and m.hl == 0xC200
            ok &= msx.get(0xC000, 512) == image[sector * 512:(sector + 1) * 512]
        check(ok, "sector reads match")
        msx.call("HWF_SEEK_FILE", hl=0, de=1000)
        msx.call("HWF_READ_FILE", hl=0x8000, bc=20000)
        check(m.a == 0 and m.bc == 20000 and msx.get(0x8000, 20000) == image[1000:21000],
              "multi chunk read (BC=%d)" % m.bc)
        pos = len(image) - 100
        msx.call("HWF_SEEK_FILE", hl=pos >> 16, de=pos & 0xFFFF)
        msx.call("HWF_READ_FILE", hl=0xC000, bc=512)
        check(m.a == 0 and m.bc == 100 and m.hl == 0xC000 + 100, "read at the end of the file (BC=%d)" % m.bc)
        msx.call("HWF_SEEK_FILE", hl=0x10, de=0)
        check(m.a == 1, "seek beyond the end gives A=1 (A=%d)" % m.a)
        msx.call("HWF_GET_FILE_ATTR")
        check(m.a == 0 and m.b == 0x20, "attributes (A=%d B=%02X)" % (m.a, m.b))

        # --- write sectors in place
        data = bytes((i * 13 + 5) & 0xFF for i in range(12000))
        msx.put(0x8000, data)
        msx.call("HWF_SEEK_FILE", hl=0, de=512 * 100)
        msx.call("HWF_WRITE_FILE", hl=0x8000, bc=12000)
        check(m.a == 0 and m.bc == 12000 and m.hl == 0x8000 + 12000, "multi chunk write (BC=%d)" % m.bc)
        msx.call("HWF_CLOSE_FILE")
        msx.cstr(S, "DISK1.DSK")
        msx.call("HWF_OPEN_FILE_DIR", hl=S)
        msx.call("HWF_SEEK_FILE", hl=0, de=512 * 100)
        msx.call("HWF_READ_FILE", hl=0xB000, bc=12000)
        check(msx.get(0xB000, 12000) == data, "written data read back")
        msx.call("HWF_CLOSE_FILE")

        # --- create, write, delete
        msx.cstr(S, "_USB")
        msx.call("HWF_CREATE_DIR", hl=S)
        check(m.a == 0, "create directory _USB")
        msx.cstr(S, "BOOTDIR")
        msx.call("HWF_CREATE_FILE", hl=S)
        check(m.a == 0, "create file")
        msx.put(0x8000, b"/GAMES")
        msx.call("HWF_WRITE_FILE", hl=0x8000, bc=6)
        check(m.a == 0 and m.bc == 6, "write the config file")
        msx.call("HWF_CLOSE_FILE")
        msx.cstr(S, "/")
        msx.call("HWF_OPEN_FILE_DIR", hl=S)
        msx.cstr(S, "_USB")
        msx.call("HWF_OPEN_FILE_DIR", hl=S)
        check(carry(m) and zero(m), "enter _USB")
        msx.cstr(S, "BOOTDIR")
        msx.call("HWF_OPEN_FILE_DIR", hl=S)
        msx.call("HWF_READ_FILE", hl=0xC000, bc=64)
        check(m.a == 0 and m.bc == 6 and msx.get(0xC000, 6) == b"/GAMES", "config file read back")
        msx.cstr(S, "BOOTDIR")
        msx.call("HWF_DELETE_FILE", hl=S)
        check(m.a == 0, "delete file")
        msx.call("HWF_OPEN_FILE_DIR", hl=S)
        check(not zero(m) and m.a == 2, "the file is gone")

        # --- DEFAULT.DSK search (DSK_DEFAULT_EXISTS uses the IY buffer)
        msx.cstr(S, "/")
        msx.call("HWF_OPEN_FILE_DIR", hl=S)
        msx.call("DSK_DEFAULT_EXISTS", iy=B)
        check(m.a == 2, "DEFAULT.DSK doesn't exist yet (A=%d)" % m.a)
        msx.cstr(S, "DEFAULT.DSK")
        msx.call("HWF_CREATE_FILE", hl=S)
        msx.call("HWF_CLOSE_FILE")
        msx.call("DSK_DEFAULT_EXISTS", iy=B)
        check(m.a == 0, "DEFAULT.DSK exists (A=%d)" % m.a)

    print("== USB floppy drive")
    mb.dev_type = DEV_FDD
    fl = mb.floppy

    cmd_read = bytes([0x28, 0, 0, 0, 0, 5, 0, 0, 3, 0, 0, 0])      # READ(10) LBA 5, 3 sectors
    msx.put(S, cmd_read)
    msx.call("USB_EXECUTE_CBI", hl=S, de=0x8000, bc=1536)           # Cy=0: receive
    check(m.a == 0 and m.bc == 1536 and m.d == 0 and m.e == 0, "READ(10) result A=%d BC=%d" % (m.a, m.bc))
    check(msx.get(0x8000, 1536) == bytes(fl.sectors[5 * 512:8 * 512]), "READ(10) data")

    cmd_read16 = bytes([0x28, 0, 0, 0, 0, 0, 0, 0, 16, 0, 0, 0])    # 16 sectors = 8192 bytes
    msx.put(S, cmd_read16)
    msx.call("USB_EXECUTE_CBI", hl=S, de=0x8000, bc=8192)
    check(m.a == 0 and m.bc == 8192 and msx.get(0x8000, 8192) == bytes(fl.sectors[:8192]), "READ(10) of 16 sectors")

    cmd_write = bytes([0x2A, 0, 0, 0, 0, 9, 0, 0, 2, 0, 0, 0])
    payload = bytes((i * 3) & 0xFF for i in range(1024))
    msx.put(S, cmd_write)
    msx.put(0x8000, payload)
    msx.call("USB_EXECUTE_CBI", hl=S, de=0x8000, bc=1024, f=1)      # Cy=1: send
    check(m.a == 0 and bytes(fl.sectors[9 * 512:11 * 512]) == payload, "WRITE(10) data reached the drive")

    msx.put(S, cmd_read)
    msx.call("USB_EXECUTE_CBI", hl=S, de=0x8000, bc=8193)
    check(m.a == 4, "transfers bigger than the buffer are refused (A=%d)" % m.a)

    fl.fail_next = (0x28, 0)        # media changed
    msx.call("USB_EXECUTE_CBI", hl=S, de=0x8000, bc=1536)
    check(m.a == 0 and m.d == 0x28 and m.e == 0, "failed command returns the sense ASC/ASCQ (D=%02X E=%02X)" % (m.d, m.e))

    fl.fail_next = (0x3A, 0)        # medium not present: with retry only for media changed
    msx.call("USB_EXECUTE_CBI_WITH_RETRY", hl=S, de=0x8000, bc=1536, a=0)
    check(m.a == 0 and m.d == 0x3A, "retry wrapper returns the ASC (D=%02X)" % m.d)

    fl.commands.clear()
    fl.fail_next = (0x28, 0)        # media changed is retried when asked to
    msx.call("USB_EXECUTE_CBI_WITH_RETRY", hl=S, de=0x8000, bc=1536, a=1)
    check(m.a == 0 and m.d == 0 and len(fl.commands) == 2, "media changed retried once (commands=%d D=%02X)" % (len(fl.commands), m.d))

    mb.dev_type = DEV_FDD
    msx.call("USB_INIT_DEV")
    check(m.a == 0, "USB_INIT_DEV recognizes the floppy drive (A=%d)" % m.a)
    msx.call("WK_HAS_CONTENTS")
    check(not zero(m), "work area has contents")
    msx.call("WK_GET_STORAGE_DEV_FLAGS")
    check(zero(m), "the floppy drive is not flagged as a storage device")
    msx.call("WK_GET_MISC_FLAGS")
    check(m.a == 0, "no hub flag")
    mb.hub = 1
    msx.call("USB_INIT_DEV")
    msx.call("WK_GET_MISC_FLAGS")
    check(m.a == 1, "hub flag set when the drive is behind a hub (A=%d)" % m.a)
    mb.hub = 0
    mb.dev_type = DEV_STORAGE
    msx.call("USB_INIT_DEV")
    check(m.a == 1, "USB_INIT_DEV: a storage device is not a floppy drive (A=%d)" % m.a)
    mb.dev_type = DEV_NONE
    msx.call("USB_INIT_DEV")
    check(m.a == 2 and m.b == RES_NO_DEVICE, "USB_INIT_DEV: no device (A=%d B=%d)" % (m.a, m.b))

    mb.dev_type = DEV_FDD
    mb.events = 1
    msx.call("USB_CHECK_DEV_CHANGE")
    check(not carry(m), "USB_CHECK_DEV_CHANGE initializes a connected floppy drive")

    # DSKCHG_IMPL on a floppy drive after media change (ASC=28h) must call GETDPB_IMPL with a valid drive number
    msx.put(0xF34D, b"\x00\xC8")                # SECBUF in page 3
    msx.put(0xD000, b"\x00" * 24)               # target DPB buffer
    fl.sectors[21] = 0xF9                       # 720 KB media byte in boot sector
    fl.fail_next = (0x28, 0)                    # TEST_DISK reports media changed (B=0xFF)
    msx.call("DSKCHG_IMPL", a=0, bc=0x00F9, hl=0xD000)
    check(not carry(m) and m.b == 0xFF and msx.get(0xD001, 1) == b"\xF9",
          "DSKCHG_IMPL updates DPB after media change (Cy=%d B=%02X dpb[1]=%02X)" % (carry(m), m.b, msx.get(0xD001, 1)[0]))

    mb.dev_type = DEV_NONE
    mb.events = 0xFF
    msx.call("USB_CHECK_DEV_CHANGE")
    check(carry(m), "USB_CHECK_DEV_CHANGE reports the removal")

    print("== error handling")
    mb.dev_type = DEV_FDD
    for i in range(72):
        mb.mem[mb.o_buf + i] = (i * 7 + 5) & 0xFF
    mb.diag = [0x01, 0x01, 0x12, 0x82, 0xE0, 0x03, 0x00, 0x08, 0x01, 0x03]
    msx.printed.clear()
    msx.call("PRINT_DIAG")
    out = bytes(msx.printed)
    check(out.startswith(b"Diag: 01 01 12 82 E0 03 00 08 01 03 \r\nHW: 05 0C 13 1A 21 28 \r\n524B443D 6E676059 "), "diagnostics line: %r" % out)
    check(out.count(b"\r\n") == 6 and out.endswith(b"F6EFE8E1 \r\n"), "hardware diagnostics lines: %r" % out)
    mb.diag = [0] * 10

    msx.printed.clear()
    msx.call("BOOT_SELFTEST_RUN")
    check(bytes(msx.printed) == b"TUR: 00 00 00\r\nFMT: 00 00 00\r\n00 00 00 20 00 00 0B 40 \r\n"
          b"CAP: 00 00 00\r\n00 00 0B 3F 00 00 02 00 \r\n",
          "boot self test output: %r" % bytes(msx.printed))
    check(m.sp == 0xEEFE + 2 or m.sp == 0xEF00, "the self test keeps the stack balanced (SP=%04X)" % m.sp)

    fl.fail_next = (0x3A, 0)
    msx.printed.clear()
    msx.call("BOOT_SELFTEST_RUN")
    check(bytes(msx.printed).startswith(b"TUR: 00 3A 00\r\nFMT: 00 00 00\r\n"),
          "self test reports the ASC of a failed command: %r" % bytes(msx.printed)[:40])
    mb.dev_type = DEV_NONE
    msx.put(S, cmd_read)
    msx.call("USB_EXECUTE_CBI", hl=S, de=0x8000, bc=512)
    check(m.a == RES_NO_DEVICE, "no device gives USB_ERR_NO_DEVICE (A=%d)" % m.a)

    mb.dev_type = DEV_FDD
    mb.delay = 100000
    msx.keys_pressed = True
    msx.call("USB_EXECUTE_CBI", hl=S, de=0x8000, bc=512)
    check(m.a == RES_PANIC, "CAPS+ESC aborts the command (A=%d)" % m.a)
    msx.keys_pressed = False
    mb.delay = 3

    print("\n%d checks, %d failures" % (checks, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
