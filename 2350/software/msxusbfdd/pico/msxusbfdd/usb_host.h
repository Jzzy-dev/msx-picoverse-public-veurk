// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// usb_host.h - USB host side of the PicoVerse USB FDD firmware (runs on core 1)
//
// Runs the TinyUSB host stack, detects the connected device and executes the mailbox commands:
// - USB floppy drives (UFI command set over CBI, CB or Bulk-Only transport) are handled by an
//   application class driver, so the MSX BIOS can send UFI commands to them
// - USB mass storage devices (SCSI, Bulk-Only) are handled by the TinyUSB MSC driver and exposed
//   to the file commands through FatFs, to access disk image (DSK) files
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef USB_HOST_H
#define USB_HOST_H

#include <stdbool.h>
#include <stdint.h>

// Core 1 entry point: initialises the USB host stack and services the mailbox commands forever
void usb_host_task(void);

// Access to the mass storage device for the FatFs disk I/O layer
bool    usb_msc_ready(void);
uint8_t usb_msc_addr(void);

// A transfer to the mass storage device timed out. The TinyUSB MSC driver can't cancel it, so the
// device is dropped until it is connected again
void    usb_msc_fault(void);

#endif // USB_HOST_H
