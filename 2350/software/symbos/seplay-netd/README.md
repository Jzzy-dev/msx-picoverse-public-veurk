# SE Play for the SymbOS Network Daemon

`seplay_netd.py` patches **SE Play 0.1** (`SEPLAY.EXE`, TMTLogic, SymbOS 4.0) so that it uses the **SymbOS Network Daemon** instead of TMTLogic's own network card (TNC / SF3). This lets the web radio page work with the PicoVerse 2350 ESP-01 daemon [`netd-esp.exe`](https://github.com/Jzzy-dev/msx-picoverse-symbos-network-daemon), or with any other SymbOS network daemon (M4, Net4CPC, DenYoNet, GR8NET...). The patch also stops SE Play hanging at start-up on machines without TMTLogic hardware.

The patcher changes your own copy of SE Play. SE Play is TMTLogic's software, so please share the patcher, not patched executables.

## Use

Only Python 3 is needed:

```sh
python seplay_netd.py SEPLAY.EXE SEPLAYN.EXE
```

Copy `SEPLAYN.EXE` next to the other SE Play files (`SE_*.QS2`, `*.WRC`, `SEPLAY.RCD`). Start the network daemon in SymbOS, then start `SEPLAYN.EXE`.

For the PicoVerse 2350, build the cartridge with `loadrom.exe -e1 -w`: SE-ONE emulation, ESP-01 Wi-Fi, Nextor and mapper in one image. See [the SE-ONE guide](../../../../docs/msx-picoverse-2350-seone.md).

The patcher checks the bytes at every patch site and refuses any file that is not SE Play 0.1, including one that has already been patched.

## What SE Play does with the network card

The findings below come from disassembling SEPLAY.EXE, which is compiled Quigs. Addresses are as linked (origin `0100h`).

- **Hardware detection at start-up (`4FECh`):** SE Play sends command `48h` to the TNC (ports `41h`/`42h`). It then reads an ID string from port `42h` until it gets a `00h` byte. Without the card that port reads `FFh`, so SE Play keeps appending characters until it runs out of string space. It runs this probe at start-up and again every time the MP3 page opens.
- **Debug output (`4E7Dh`):** every debug message, including each AT command, is written character by character to port `40h`. That is the TNC console. On an MSX it is also the switched-I/O device ID register.
- **Web radio (`1885h`):** stations come from `*.WRC` files and are parsed with fixed columns (`1ACDh`):
  - the IP is stored as 4 words at `7740h`;
  - the port is stored as a high byte at `774Ah` and a low byte at `774Ch`;
  - the stream path, short name and request are kept in string variables;
  - the request is `GET <path> HTTP/1.1` plus an empty line, with no `Host:` header.
- **Web radio connection:** the TNC routines do the work:
  - open at `50CFh` (result `0`/`1` in `7555h`),
  - send the request string `75B9h` at `518Bh`,
  - close at `5221h`,
  - bytes waiting at `5272h` (count in `75A9h`).

  The timer tick (`1D40h`) then runs three times per tick. Each run checks the byte count, and while SE-ONE port `21h` reads below `75h` it copies the bytes from the TNC data port to SE-ONE port `22h` with `IN`/`OUT` (loop at `1D7Eh`).
- **Phone (VoIP), IR remote and SF3 streaming:** these speak the TNC command protocol (ports `40h`-`49h`) and need TMTLogic's hardware.

## What the patch changes

The patcher appends a 1818-byte block (`netd_seplay.asm`) to the code segment. It then relinks the executable using its SymbOS relocation table:

- every relocated pointer into the data and transfer segments moves by the block size, as do the header's code length, relocation count and 16-colour icon offset;
- the block's own 109 relocation entries are found by assembling it twice at addresses 1000h apart.

| Address | Was | Now |
| --- | --- | --- |
| `4FECh` | TNC ID probe | `net_find`: hardware ID `""`, finds the daemon with `SySystem_PRGSRV` (`"Network Daem"`) |
| `50CFh` | TNC open | `net_open`: `TCPOPN` to the station IP/port, then waits for "established" with `TCPSTA` |
| `518Bh` | TNC send | `net_send`: sends the request line plus `Host:` and `User-Agent:` headers with `TCPSND` |
| `5221h` | TNC close | `net_close`: `TCPCLO` |
| `5272h` | TNC bytes waiting | `net_rxavail`: `TCPSTA`, capped to the 1 KB buffer |
| `1D7Eh` | `IN` TNC port → `OUT` port 22h | `net_pump`: `TCPRCV` into the buffer, then `OUT` to port `FF22h` (SE-ONE stream; also right on CPC) |
| `1D08h` | drain TNC data | `RET` |
| `4E7Dh` | debug text to port `40h` | `RET` |
| `2A45h` | phone page | shows `PHONE: NO TNC` |

Daemon requests are SymbOS messages:

- **Request:** sent with `RST #10`. The function is in byte 0; registers AF, BC, DE, HL, IX and IY are at offsets 2-13.
- **Answer:** fetched with `RST #18`, filtered on the daemon's process ID. Desktop messages stay queued for SE Play's main loop.
- **Waiting:** between polls the code gives up its time slice with `RST #30`. Unsolicited `MSR_NET_TCPEVT` messages are skipped.
- **Timeouts:** every wait has one. If the daemon is missing, the station can't be opened and SE Play shows `URL ERROR...`. If the daemon starts later, the next station click finds it.

The FM, MP3, M3U and synthesizer pages are unchanged. With the hardware ID empty, SE Play always uses stream mode 1, where the MSX pushes the data.

## Tests

`2350/software/loadrom.pio/pico/loadrom/seone/tests/z80_netd.c` runs the patched executable in a Z80 emulator. The executable is loaded and relocated the way the SymbOS loader does it, alongside:

- a minimal SymbOS message kernel,
- a system manager,
- a simulated network daemon that streams an MP3 file as "web radio",
- the PicoVerse SE-ONE emulation.

It checks:

- start-up,
- the MP3 page,
- WRC parsing,
- connecting,
- the HTTP request,
- several MB of stream decoded,
- stopping,
- the phone page,
- running without a daemon and with a daemon started later,
- that ports `40h`-`49h` are never accessed.

```sh
make Z80DIR=<superzazu/z80> SEPLAY_NETD=SEPLAYN.EXE USB_FILES=<SE-ONE USB_FILES> netd
```

## Rebuilding the block

After changing `netd_seplay.asm`, run:

```sh
python seplay_netd.py --rebuild --sjasmplus <path to sjasmplus>
```

This writes `netd_block.json`. To assemble directly while patching, pass `--sjasmplus`; the result is byte-identical.
