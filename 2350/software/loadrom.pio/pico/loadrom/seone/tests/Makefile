# Host test for the SE-ONE emulation.
#   make FATFS=<no-OS-FatFS-SD-SDIO-SPI-RPi-Pico>/src MP3=<picomp3lib>/src USB_FILES=<SE-ONE USB_FILES dir> run
.DEFAULT_GOAL := host_seplay
FATFS     ?= ../../lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico/src
MP3       ?= ../../lib/picomp3lib/src
USB_FILES ?= USB_FILES

CFLAGS = -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -DSEONE_HOST_TEST \
         -I.. -Ihostinc -I$(FATFS)/include -I$(FATFS)/ff15/source -I$(MP3)

SEONE  = ../seone_bus.c ../seone_cmd.c ../seone_audio.c
FATSRC = $(FATFS)/ff15/source/ff.c $(FATFS)/ff15/source/ffunicode.c $(FATFS)/ff15/source/ffsystem.c
MP3SRC = $(filter-out $(MP3)/buffers.c,$(wildcard $(MP3)/*.c))

# same FatFS configuration as the firmware, plus f_chmod() for building the test image
hostinc/ffconf.h: $(FATFS)/include/ffconf.h
	mkdir -p hostinc
	sed 's/^#define FF_USE_CHMOD.*/#define FF_USE_CHMOD 1/' $< > $@

host_seplay: hostinc/ffconf.h host_seplay.c host_platform.c buffers.o $(SEONE)
	$(CC) $(CFLAGS) -o $@ host_seplay.c host_platform.c buffers.o $(SEONE) \
	    $(FATSRC) $(MP3SRC) -w -lm

buffers.o: $(MP3)/buffers.c hostinc/ffconf.h
	$(CC) $(CFLAGS) -w -DMPDEC_ALLOCATOR=seone_mp3_alloc -include ../seone_mp3_alloc.h -c $< -o $@

run: host_seplay
	./host_seplay $(USB_FILES)

clean:
	rm -rf host_seplay z80_seplay buffers.o test.img out.wav hostinc

# Optional: execute SE Play's own Z80 code against the emulation.
#   make Z80DIR=<superzazu/z80 checkout> SEPLAY=<path to SEPLAY.EXE> z80
Z80DIR ?= z80
SEPLAY ?= SEPLAY.EXE
z80_seplay: z80_seplay.c host_platform.c buffers.o $(SEONE) hostinc/ffconf.h
	$(CC) $(CFLAGS) -I$(Z80DIR) -o $@ z80_seplay.c $(Z80DIR)/z80.c host_platform.c buffers.o $(SEONE) \
	    $(FATSRC) $(MP3SRC) -w -lm

z80: host_seplay z80_seplay
	./host_seplay $(USB_FILES)
	./z80_seplay $(SEPLAY) test.img
