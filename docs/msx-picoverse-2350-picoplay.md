# PicoPlay: a SymbOS music player for the MSX PicoVerse 2350

LoadROM's `-p1` firmware gives the PicoVerse 2350 its own audio engine, the **PicoVerse Audio Engine**. The cartridge reads MP3, WAV and M3U files from its microSD card, decodes them and plays them through its I2S DAC. **PicoPlay** is the SymbOS program that browses the card and controls playback. **PVTEST.COM** checks the engine from the MSX-DOS / Nextor prompt.

> **Status:** confirmed on a Philips NMS 8245 with the PicoVerse 2350: Nextor boots, PVTEST plays folders, and PicoPlay under SymbOS 4.0 browses, plays, pauses, skips, shuffles, repeats and sets the volume. Internet radio (section 6) is new and tested only against a simulated ESP so far. Also tested in openMSX with a model of the engine, and with PC tests against the real engine code.

## Why this design

Earlier attempts streamed audio through the MSX (MP3MSX-style, with hiccups) or copied the SE-ONE's I/O-port protocol (SE Play). Both depend on the MSX bus keeping up:

- **No audio crosses the MSX bus.** The RP2350 reads the files from the microSD card itself and decodes them on core 1. The MSX only sends short commands and reads a status block, so a busy MSX can't cause hiccups.
- **Memory-mapped, not I/O ports.** The PicoVerse answers I/O reads without /WAIT, so a late answer reads as `FFh`. The engine instead sits in a memory window. Every memory access holds the Z80 with /WAIT until the RP2350 has answered, so no byte can be lost. The SymbOS network daemon reaches the Wi-Fi interface the same way, and that method is confirmed on hardware.
- **The MSX never waits blindly.** Commands are posted and checked later. Every wait gives the CPU back to SymbOS and has a time limit. A missing or stuck cartridge shows a message; it can't freeze SymbOS.
- **Diagnostics built in.** The engine keeps a heartbeat, the microSD mount result, FatFS error codes and a log. PicoPlay and PVTEST show them, so a problem on real hardware can be traced.

## 1. What you need

- A PicoVerse 2350 with a microSD card: MBR partition table, first primary partition FAT16. This is the same card Nextor and SymbOS use.
- An MSX2 or later. SymbOS 4.0 is installed on the card as in the [SymbOS guide](./msx-picoverse-2350-symbos.md).
- `PICOPLAY.EXE` and `PVTEST.COM`, built from `2350/software/symbos/picoplay`.

## 2. Build and flash

```powershell
.\2350\software\loadrom.pio\tool\dist\loadrom.exe -p1 -w -o picoplay_wifi.uf2   # with the ESP-01 Wi-Fi
.\2350\software\loadrom.pio\tool\dist\loadrom.exe -p1 -o picoplay.uf2           # without Wi-Fi
```

The image holds Nextor 2.1.4 (Sunrise IDE on the microSD card), the 1 MB PSRAM mapper, the audio engine and, with `-w`, the Wi-Fi interface. The sub-slots are the same as in the `-m1 -w` SymbOS setup, with or without `-w`:

| Sub-slot | Contents |
| --- | --- |
| 0 | ESP8266 BIOS + memio UART (with `-w`, otherwise empty) |
| 1 | Nextor + Sunrise IDE |
| 2 | 1 MB mapper RAM |
| 3 | PicoVerse Audio Engine, window `4000h-4FFFh` |

The SymbOS disk driver stays on slot `P-1`, and the network daemon still finds Wi-Fi in `P-0`.

## 3. Music on the card

Put the music anywhere on the first partition, for example `A:\MUSIC\`, with sub-folders and `.M3U` play lists.

- **Formats:** MP3 (MPEG-1/2/2.5 layer III, CBR and VBR) and PCM WAV (8/16-bit, mono/stereo, 8 to 48 kHz).
- **Tags:** title, artist and album from ID3v2.3, ID3v2.4 or ID3v1. Without a tag, the file name is shown. Accented letters are shown without their accents.
- **Length and seeking:** from the Xing/Info or VBRI header (VBR) or the bitrate (CBR).
- **Names:** long file names work, including non-ASCII ones; folders are opened by position, not by retyping the name. Lists are sorted with folders first. Hidden files, `._` files and other file types are left out.
- **M3U:** relative paths, absolute paths, and Windows paths such as `C:\Music\Song.mp3`. When the full path isn't found, the engine looks for the file name in the play list's folder. `http://` and `https://` lines are internet radio stations (see section 6), named by the `#EXTINF` line above them. Comments and empty lines are skipped.
- **Sizes:** up to 1024 entries per folder (PicoPlay shows them in pages of 120) and 512 tracks per play list.

## 4. PVTEST.COM (MSX-DOS / Nextor)

A quick check of the cartridge, without SymbOS:

```text
PVTEST                     find the engine, show diagnostics and the root folder
PVTEST /MUSIC              list a folder and play its tracks
PVTEST /MUSIC/SONG.MP3     play a file (or an .M3U play list)
PVTEST SONG.MP3            relative to the current folder (CD MUSIC first)
PVTEST A:\MUSIC\SONG.MP3   with a drive letter and backslashes
PVTEST http://host/stream  play an MP3 internet radio stream (needs -p1 -w)
```

For a stream the status line shows the time, the buffer fill (`B95%`), the receive rate in KB/s and the stream title. `Buf` means the buffer is still filling. A stream that fails shows `net error N HTTP H`, and `L` shows the engine log.

While playing: `SPACE` pause, `N` next, `P` previous, `S` stop, `+`/`-` volume, `L` engine log, `ESC` quit. After you quit, the music keeps playing: the cartridge plays it, not the MSX.

## 5. PicoPlay (SymbOS)

Copy `PICOPLAY.EXE` to the SymbOS drive and start it from SymbOS.

- **Folders:** double-click a folder (`[name]`) to open it, and `[..]` to go up. Folders with more than 120 entries show `>> more entries` / `<< previous entries` rows. *File > Refresh folder* reads the folder again. The engine remembers the last folder, even after PicoPlay is closed.
- **Playing:** double-click a track (or select it and press Enter) to play it. The folder's tracks become the play list. Double-click an `.M3U` file to play it as a list.
- **Controls:** `|<` previous (restarts the track after 3 s), `Play`/`Pause`, `Stop`, `>|` next, *Shuffle*, *Repeat*, the volume slider, and a click on the progress bar to seek.
- **Keys:** `Up`/`Down` select (`Left`/`Right` a page), `Enter` open or play, `Backspace` parent folder, `SPACE` play/pause, `N` next, `P` previous, `S` stop, `+`/`-` volume, `D` diagnostics, `L` engine log.
- **Status:** the window shows title, artist and album, the time, format, bitrate and sample rate. The status bar shows the play list position or the latest problem in plain words.
- **Diagnostics:** *File > Diagnostics* shows the engine version, uptime, heartbeat, microSD mount result, volume type, play list and errors. *File > Engine log* shows the engine's last 32 log lines. *File > Remount microSD* mounts the card again.
- **Music keeps playing** when you close PicoPlay; start it again to take control.

PicoPlay finds the engine by scanning the slots for its signature. It accesses the cartridge with the same slot method as the PicoVerse network daemon: page 1 is switched briefly with interrupts off, in blocks of at most 128 bytes.

## 6. Internet radio

With a `-p1 -w` image the engine plays MP3 internet radio streams itself. The ESP-01 fetches the stream (WiFi, DNS, TCP and TLS run on the ESP), the engine decodes it, and the MSX only shows what is playing. The ESP must already know your access point (as for the Nextor WiFi tools).

- **Station lists:** put stations in an `.M3U` file on the card and open it in PicoPlay, or use `PVTEST http://...`:

  ```text
  #EXTM3U
  #EXTINF:-1,Radio Paradise
  http://stream.radioparadise.com/mp3-128
  #EXTINF:-1,SomaFM Groove Salad
  https://ice1.somafm.com/groovesalad-128-mp3
  ```

  `N` and `P` (or `>|` and `|<`) switch stations. `examples/RADIO.M3U` has a few to start with.
- **What plays:** MP3 streams over `http://` and `https://` (Icecast, Shoutcast). Redirects and `.pls` / `.m3u` station links are followed. AAC, Ogg and HLS (`.m3u8`) streams are refused with a clear message. Radio Browser (radio-browser.info) lists stations with their codec; pick ones marked MP3.
- **What you see:** the stream title (split into artist and title), the station name, the bitrate, and the buffer fill. The progress bar shows the buffer fill. The status bar shows the receive rate, or `connecting` / `reconnecting`.
- **Buffer:** 96 KB (6 s at 128 kbit/s). Playback starts after 24 KB. When the connection drops or the server stops sending for 10 s, the engine reconnects by itself while the buffer keeps playing. Pause keeps the connection and fills the buffer.
- **The WiFi link:** while a stream plays (and for a moment after Stop), the engine owns the ESP. The SymbOS network daemon and the Nextor WiFi tools cannot use it at the same time. Stop the stream first.
- **HTTPS:** the ESP does TLS 1.2 with SNI, one TLS connection at a time. The certificate is not checked. The handshake takes one or two seconds.
- **Diagnostics:** *File > Diagnostics* shows the host, TLS, HTTP status, receive rate, buffer, reconnects, longest receive gap, underruns, DNS and open times, and ESP errors. These numbers are what to send back when a station does not play well.

## 7. Register window (for developers)

The engine is in page 1 of sub-slot 3, `4000h-4FFFh`. The full definition is `2350/software/loadrom.pio/pico/loadrom/pvaudio/pva_proto.h`.

| Address | Size | Contents |
| --- | --- | --- |
| `4000h` | 16 | `"PVAUDIO"`, 0, API version, firmware major, minor |
| `4010h` | 1 | command token (write the new token first) |
| `4011h` | 1 | command code (writing it posts the command) |
| `4012h` | 1 | token of the last finished command |
| `4013h` | 1 | result (`0` = OK) |
| `4014h` | 2 | reply length |
| `4016h` | 1 | heartbeat, +1 at least every 10 ms while the engine runs |
| `4020h` | 224 | status block; byte 0 is odd while it is being updated |
| `4100h` | 512 | command arguments |
| `4400h` | 2048 | command reply |

Commands: `PING`, `MOUNT`, `LOG`, `DIAG`, `DIR_OPEN path`, `DIR_READ first,count`, `DIR_ENTER index`, `PLAY_INDEX index`, `PLAY_PATH path`, `PAUSE`, `STOP`, `NEXT`, `PREV`, `SEEK seconds`, `VOLUME 0-100`, `MODE repeat/shuffle`, `PLAY_URL url,0,station,0` (also `PLAY_PATH` with a URL).

For streams the status block has the `STREAM` and `BUFFERING` flags, and from byte 162: network state, buffer fill in %, receive rate in 0.1 KB/s, reconnects, the last network error (`PVA_NERR_xxx`), the last HTTP status and the buffer size in KB. Results `9` (network) and `10` (no WiFi link) are new.

To send a command, write the arguments, then a new token, then the code. On a later check, compare the done token with your token, then read the result and the reply. To read the status block, read byte 0, copy the block, and read byte 0 again; if it changed or is odd, copy again.

## 8. Sources and tests

- Engine: `2350/software/loadrom.pio/pico/loadrom/pvaudio/` (`pva_window.c` runs on core 0; `pva_engine.c`, `pva_decode.c`, `pva_net.c` and `pva_hal_pico.c` run on core 1; the ESP link functions are in `loadrom.c`). LoadROM mode: `loadrom_sunrise_mapper_pva_sd()`, ROM type 22.
- Host test: `pvaudio/tests`, `make run`. It builds a FAT16 image and drives the window as PicoPlay does. It covers folders, real-time playback with no underruns while browsing, tags, VBR, mono, WAV, seek, volume, shuffle, repeat, M3U and a removed card. `tests/host_esp.c` simulates the ESP-01 (its UNAPI protocol, UART speed, DNS and TLS delays) with test radio servers, for streams: ICY titles, pause, dropped connections, a stalled and a slow server, redirects, `.pls`, HTTPS, AAC, 404, unknown hosts, no WiFi, and switching stations.
- PicoPlay and PVTEST: `2350/software/symbos/picoplay/` (SymbOS C Compiler and sjasmplus). `test/host_app.c` runs PicoPlay's logic against the engine. It covers browsing, paging, playing, seeking, the controls, diagnostics, a stuck engine and a missing engine. `test/z80_pvtest.c` runs PVTEST.COM in a Z80 emulator.
- openMSX: `picoplay/emu/` has an openMSX device that runs the engine behind the window, plus an extension with the cartridge's slot layout, for testing PicoPlay under real SymbOS.

## 9. Next steps

- Internet radio on real hardware: receive rate, gaps and TLS times from Diagnostics.
- Browsing Radio Browser (by country, genre and decade) from PicoPlay, with favourites saved as M3U.
- AAC streams, if the RP2350 has the headroom.
