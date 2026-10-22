# MSX PicoVerse 2350 SE-ONE Emulation (SE Play for SymbOS)

The LoadROM `-e1` firmware turns the PicoVerse 2350 into a **TMTLogic SE-ONE** MP3 cartridge on top of the usual **Nextor Sunrise IDE (microSD) + 1 MB mapper** setup. Software written for the SE-ONE, in particular **SE Play** for SymbOS 4.0, can browse the microSD card, play MP3 and WAV files and M3U playlists, and set volume, bass and treble. Audio goes out through the cartridge's I2S DAC.

This works the same way earlier PicoVerse firmware stood in for the Sunrise MP3 cartridge: the MSX talks to the same I/O ports as on the original hardware, and the RP2350 does the work of the SE-ONE's ARM controller, VS1053 decoder and TDA8425 audio processor.

> **Status:** the firmware builds and has been tested on a PC. The tests feed SE Play's own Z80 code through the emulated ports and decode real MP3 files, and they cover start-up, browsing, playing and playlists. It **has not been confirmed on real MSX hardware yet.** Please report results.

## 1. What you need

- A PicoVerse 2350 with a microSD card.
- An MSX2 or later with SymbOS 4.0 installed on the card, using the PicoVerse-patched disk driver. Follow the [SymbOS guide](./msx-picoverse-2350-symbos.md). The `-e1` mode boots exactly like the `-m1` mode described there.
- SE Play (`SEPLAY.EXE` and its `.QS2`, `.WRC` and `.RCD` files) from the SymbOS software archive.
- Optional: the SE-ONE speech files (`SETALK`) and sample files (`SAMPLE`) from the TMTLogic SE-ONE package.

## 2. Build and flash

From the repository root on a Windows PC:

```powershell
.\2350\software\loadrom.pio\tool\dist\loadrom.exe -e1 -o seone_symbos.uf2
```

Flash the UF2 with the normal BOOTSEL procedure. The image holds Nextor 2.1.4 (Sunrise IDE on the microSD card), the 1 MB PSRAM memory mapper and the SE-ONE emulation. You don't need any extra files on the cartridge.

## 3. Prepare the microSD card

Use the same card as SymbOS: MBR-partitioned, with a first primary **FAT16** partition. The SE-ONE emulation reads the first FAT partition, the one Nextor and SymbOS use, so your music lives on the same drive:

```text
A:\SYMBOS\          SymbOS + SE Play (SEPLAY.EXE, SE_*.QS2, WEBRADIO.WRC, SEPLAY.RCD)
A:\MUSIC\           your MP3/WAV files, sub-folders and .M3U playlists
A:\SETALK\          SP000H.MP3 ... SP03FH.MP3   (optional, AT+MSDTALK speech)
A:\SAMPLE\          BELL.MP3, ...                (optional, AT+MSDSAMPLE samples)
```

- **Formats:** MP3 (MPEG-1/2/2.5 layer III, any bitrate, ID3v2 tags skipped) and PCM WAV (8/16-bit, mono/stereo, 8 to 48 kHz).
- **Names:** SE Play strips every space from a command before sending it, which is why the real SE-ONE only works with 8.3 names. This firmware matches names case-insensitively in several ways:
  - the long name,
  - the long name with its spaces removed,
  - the 8.3 short name,
  - the short name SE Play derives itself from playlist rows.

  In practice, normal long file names work. Lists show names of up to 20 characters as-is and fall back to the 8.3 name for longer ones. Extensions are shown in upper case, because SE Play recognises playlists by `.M3U`.
- **Filtering:** listings show folders, `.MP3`, `.WAV` and `.M3U` only. Hidden and system entries and macOS `._` files are left out. SE Play keeps at most 49 entries per folder, so split very large folders.
- **Playlists:** keep the `.M3U` next to the songs, as described in the SE Play README. Rows with sub-folders (`bonus/03 track.mp3`) and Windows absolute paths (`C:\...\song.mp3`) also resolve: the firmware uses the file name, relative to the playlist's folder.

## 4. Use SE Play

1. Boot the MSX into Nextor, start SymbOS and run `SEPLAY.EXE`.
2. On the **MP3** page, SE Play mounts the card: it sends `AT+USBINIT`, and the root folder appears. Open folders and pick a file to play. Pause, Stop, volume, bass and treble work, and the VU meter follows the music.
3. On the **M3U** page, the playlists are listed. Pick one to play it in order, shuffled or repeated, using SE Play's own play-mode setting.

Connect the PicoVerse audio output to your amplifier. The SE-ONE could mix the MSX's own sound into its output (`AT+MSXAUDIO`). The PicoVerse has no analogue input, so that command is accepted but does nothing.

### What works and what does not

| SE Play page / feature | On PicoVerse |
| --- | --- |
| MP3 (USB) browsing, play, pause, stop, VU, volume/bass/treble | **Yes** (microSD instead of a USB stick) |
| M3U playlists | **Yes** |
| WAV files | **Yes** |
| FM radio | **No**: there is no tuner. Commands are accepted, the frequency display changes, and there is no sound. |
| Web radio | **Yes, with the patched SE Play** (`SEPLAYN.EXE`, see section 5) and the PicoVerse network daemon: build with `-e1 -w` and fit an ESP-01. The original SE Play needs TMTLogic's network card. |
| Phone (VoIP), users online | **No**: these need TMTLogic's network card (TNC/SF3) on ports `0x40`-`0x49`. The patched SE Play shows `PHONE: NO TNC`. |
| Synthesizer | **No**: this needs the SF3's General MIDI synth. MIDI bytes sent to port `0x22` are ignored. |
| IR remote learning | **No**: RSF3 only. |
| `AT+MSDTALK` (SP0256 speech) and `AT+MSDSAMPLE`, for BASIC programs | **Yes**, from `/SETALK` and `/SAMPLE` |

**About the network card (unpatched SE Play):** SE Play checks for the TNC at start-up by reading an ID string from port `0x42` until it gets a `0x00` byte. On an MSX without the card that port reads `0xFF`, so SE Play would spin until its string space runs out. Once software writes to the TNC ports, the firmware answers with "idle, empty ID", and SE Play carries on with no network.

## 5. Web radio: SE Play patched for the SymbOS network daemon

SE Play 0.1 drives TMTLogic's network card directly. `2350/software/symbos/seplay-netd` contains a patcher that changes your copy of `SEPLAY.EXE` so it uses the **SymbOS Network Daemon** instead, for example the PicoVerse ESP-01 daemon [`netd-esp.exe`](https://github.com/Jzzy-dev/msx-picoverse-symbos-network-daemon). The patch also stops the start-up hang for good, because SE Play no longer touches ports `0x40`-`0x49`. See the [patcher README](../2350/software/symbos/seplay-netd/README.md) for details.

1. Build the firmware with Wi-Fi: `loadrom.exe -e1 -w -o seone_wifi.uf2`. This uses the same sub-slot layout as the other `-w` modes: ESP8266P BIOS, then Nextor, then mapper. Fit and configure the ESP-01 as described in the [Wi-Fi guide](./msx-picoverse-2350-wifi.md).
2. On a PC, run `python seplay_netd.py SEPLAY.EXE SEPLAYN.EXE` and copy `SEPLAYN.EXE` next to the other SE Play files.
3. In SymbOS, start `netd-esp.exe` and wait until it shows an IP address. Then run `SEPLAYN.EXE` and open the **Web radio** page.

Stations come from the `.WRC` files in the SE Play folder:

```text
089.147.108.034	|8000	|/radio.mp3	|PIGPEN	|www.pigpenradio.org	|Reggae	|dub	|UK|
```

- **IP address:** write it with three digits per number (`089.147.108.034`). SE Play reads these as fixed columns.
- **Port:** follows the first `|`.
- **Stream path:** follows the second `|`.
- **Short name:** follows the third `|`, and is what the display shows.

**Limits:**

- **MP3 only.** Stations must send MP3; AAC streams stay silent.
- **No host names or redirects.** Stations are reached by IP address only, and HTTP redirects are not followed. The station lists shipped with SE Play date from 2025, so expect to update the IP addresses.
- **HTTPS:** the PicoVerse daemon opens connections to port 443 as TLS. Without a host name, though, many servers refuse the connection.

## 6. Port and command reference

### I/O ports

| Port | Read | Write |
| --- | --- | --- |
| `0x20` | AT reply, one byte per read; `0x00` while nothing is pending | AT command characters, ended by CR |
| `0x21` | MP3A/MP3B: stream FIFO fill (256-byte units); FM: RDS ready (always 0) | - |
| `0x22` | - | MP3 byte stream (web radio path); SYNTH: MIDI (ignored) |
| `0x23` | Status: bit 3 no music playing, bit 4 storage error, bit 5 speech/sample busy, bit 6 FIFO above high level, bit 7 FIFO below low level | - |
| `0x25` | Directory listing after `AT+MSDFILES`: `NAME<TAB>` entries, folders prefixed `/`, then `<LF>`, then `0x00` | - |
| `0x26` | MP3B: left VU 0-8; REG: state (0 idle, 1 busy, 2 error) | REG: `0` resets the data pointer, any other value runs that function |
| `0x27` | MP3B: right VU 0-8; REG: next result byte | REG: function data byte |
| `0x41`-`0x49` | TNC stub: `0x00` once armed (never port `0x40`, never while switched-I/O ID 8 is selected) | arms the stub |

TEST mode reads back the last value written to `0x21`-`0x27`, as described in the manual.

### AT commands

Queries answer `AT+NAME=value<CR><LF>OK<CR><LF>`. Actions answer `OK<CR><LF>`, or `ERR` or `ILL`. `AT+NAME/H` returns a help line, and `AT+LIST` lists every command. Before running a command, the firmware drops anything left over from the previous reply.

- **System:** `CARTRIDGE?` (`SEONE/M`), `SEMODE=MP3A|MP3B|FM|REG|TEST|MIDI|SYNTH|VOICE` and `?`, `SEVERSION?`, `SEFIRMWARE?`, `SEFIRMWARECHK=DDMMYYYY`, `SELOGBOOK?`, `SEFAULTNR?`, `SEFAULTTXT?`, `SETUNE`, `SEPLAYTUNE`.
- **Storage:** `USBINIT`, `USBDEINIT`, `USBDEVICE?`, `MSDPATH=0:/DIR` (`0:` and `1:` both mean the microSD card) and `?`, `MSDPATHUP`, `MSDFILES`.
- **Playback:** `MSDPLAY=name` and `?` (`BUSY`/`IDLE`), `MSDPAUSE`, `MSDSTART`, `MSDSTOP`, `MSDSIZE?`, `MSDCNT?`, `MSDM3UFILE=path`, `MSDM3UGETROW=n` (1-based, `#END#` past the end).
- **Speech and samples:** `MSDTALK=0..63`, which queues `SPxxxH.MP3` allophones. `MSDSETSAMPLE=n@FILE.MP3` (or `MSDSAMPLE=n@FILE`) stores a sample in slot n, and `MSDSAMPLE=n` plays it.
- **Audio:** `DSPVOLL`, `DSPVOLR`, `DSPBASS`, `DSPTREBLE` (0-100; 50 is flat for bass and treble), `DSPVOLUP`, `DSPVOLDOWN`, `DSPMUTE`, `DSPTYPE?`, `MP3TYPE?`, `MP3LOWLEVEL`, `MP3HIGHLEVEL`.
- **VU:** `VULEFT?`, `VURIGHT?` (peak byte), `VULN?`, `VURN?` (LED count), `VULEDS=1..32`.
- **FM (stubs, no tuner):** `FMFREQ`, `FMSCANUP`, `FMSCANDOWN`, `FMGETRDS`, `FMSTARTFREQ`, `FMTYPE?` (`NONE`) and the other `FM*` settings, which are stored and read back.

### REG mode functions

The firmware implements the manual's REG mode functions:

- **DSP and volume:** `16`/`17` DSP select, `48`/`49` volume up/down, `50`/`51` volume, `52`/`53` bass/treble/stereo/mute.
- **FM (stubs):** `32`-`38`.
- **VU:** `64`-`70`.
- **Firmware:** `0x60` returns the firmware date `20250306`.
- **Storage and playback:** `0x70` init, `0x72` available, `0x74` play, `0x75` get name, `0x76`/`0x77` path, `0x78` start, `0x79` stop, `0x7A` pause, `0x7B` file list, `0x7C` size, `0x7D` percentage.
- **Speech and samples:** `126` talk, `127`/`128` samples.

The FAT32 sector functions (`0x50`-`0x54`) are not available.

## 7. How it works

- **Core 0** runs the expanded-slot bus loop: Nextor on sub-slot 0, the mapper on sub-slot 1, plus the SE-ONE ports. The Z80 does not wait on I/O reads, so the loop answers them between every memory write. An OUT that is still in the write FIFO is applied before the IN that follows it. The port handlers run from RAM and never block (`seone/seone_bus.c`).
- **Core 1** alternates between three jobs:
  - Nextor's raw sector requests to the card,
  - the SE-ONE command processor (`seone/seone_cmd.c`, FatFS),
  - the audio engine (`seone/seone_audio.c`), which decodes at most one frame when an I2S buffer is free.

  It runs on its own 16 KB stack, because the default 2 KB is too small for the file-name handling.
- **Memory:** the SE-ONE takes its buffers, the Helix MP3 decoder state (about 24 KB) and the core 1 stack from the 128 KB of the SRAM ROM cache that the Nextor kernel doesn't use.
- **Card access:** the emulation only ever reads the card. Nextor/SymbOS write to it directly, so FatFS's sector cache is dropped before every command.

## 8. Building from source

The firmware is part of LoadROM (`2350/software/loadrom.pio/pico/loadrom`). Besides the existing `lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico`, it needs the Helix decoder in `lib/picomp3lib` ([ikjordan/picomp3lib](https://github.com/ikjordan/picomp3lib)), with the same layout Explorer uses. Build with Pico SDK 2.2.0 and pico-extras as usual. After rebuilding `dist/loadrom.bin`, regenerate `tool/src/loadrom.h` with `xxd -i` and rebuild the tool.

## 9. Tests

`2350/software/loadrom.pio/pico/loadrom/seone/tests` contains a host test harness:

- `make run`: builds a FAT16 image with FatFS and drives the ports the way SE Play does, covering start-up, the two-pass listing, folders with spaces, playback to end, pause, M3U, WAV, the stream FIFO, speech, samples and REG mode.
- `make z80 Z80DIR=<superzazu/z80> SEPLAY=<SEPLAY.EXE>`: runs SE Play's own compiled Z80 routines against the emulation in a Z80 emulator. SEPLAY.EXE is not included.

The decoded output was also compared against ffmpeg. After aligning the first frame, the largest difference was 2 LSB.
