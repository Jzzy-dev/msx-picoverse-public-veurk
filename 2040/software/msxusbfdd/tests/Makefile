######################################################################
# MSX PICOVERSE PROJECT
# (c) 2026 Cristiano Goncalves
# The Retro Hacker
#
# Makefile - host tests of the PicoVerse USB FDD firmware
#
# test:    builds and runs the file system service test on the PC (needs gcc).
# z80test: runs the routines of the BIOS ROM in a Z80 emulator against the real
#          file system service (needs gcc, Python and "pip install z80").
######################################################################

CC := gcc
PYTHON ?= python
PICODIR := ../pico/msxusbfdd
BINDIR := build

ifeq ($(OS),Windows_NT)
LIBFILE := $(BINDIR)/pvhost.dll
else
LIBFILE := $(BINDIR)/libpvhost.so
endif

CFLAGS := -g -Wall -Wno-unused-function -DFATFS_HOST_TEST -I$(PICODIR) -I$(PICODIR)/fatfs
FS_SOURCES := $(PICODIR)/fs_service.c $(PICODIR)/fatfs/ff.c

.PHONY: all test z80test clean

all: test z80test

test: $(BINDIR)/fs_service_test.exe
	./$(BINDIR)/fs_service_test.exe

z80test: $(LIBFILE)
	$(PYTHON) z80_mailbox_test.py

$(BINDIR)/fs_service_test.exe: fs_service_test.c $(FS_SOURCES)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $^ -o $@

$(LIBFILE): pv_host_lib.c $(FS_SOURCES)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -shared $^ -o $@

clean:
	rm -rf $(BINDIR)
