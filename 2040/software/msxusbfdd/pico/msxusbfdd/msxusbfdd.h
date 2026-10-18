// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// msxusbfdd.h - Pin definitions of the PicoVerse 2040 MSX cartridge
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef MSXUSBFDD_H
#define MSXUSBFDD_H

// Address lines (A0-A15)
#define PIN_A0     0
#define PIN_A15    15

// Data lines (D0-D7)
#define PIN_D0     16
#define PIN_D7     23

// Control signals
#define PIN_RD     24   // Read strobe from MSX
#define PIN_WR     25   // Write strobe from MSX
#define PIN_IORQ   26   // IO Request line from MSX
#define PIN_SLTSL  27   // Slot Select for this cartridge slot
#define PIN_WAIT   28   // WAIT signal to the MSX
#define PIN_BUSSDIR 29  // Bus direction signal

#define ROM_SIZE        32768   // The BIOS is a 32KB ROM: two 16KB banks, four 8KB banks of the mapper
#define ROM_BANK_SIZE   8192

#endif // MSXUSBFDD_H
