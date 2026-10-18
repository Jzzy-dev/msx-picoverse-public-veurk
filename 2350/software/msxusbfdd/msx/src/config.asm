; MSX PICOVERSE 2040 - USB FDD BIOS
; Based on the Rookie Drive USB FDD BIOS created by Konamiman in 2018
; and on the MSXUSB USB FDD BIOS (CH376 version) by The Retro Hacker
; (c) 2026 Cristiano Goncalves
; The Retro Hacker
;
; This is the customization options file.
; Flags are enabled with value 1 or disabled with value 0.


; -----------------------------------------------------------------------------
; Behavior configuration flags
; -----------------------------------------------------------------------------

;Invert the behavior of the CTRL flag, so that
;the second "ghost" drive exists only if CTRL is pressed at boot time
;(this won't apply to the internal disk drive or other MSX-DOS kernels)
INVERT_CTRL_KEY: equ 0

;When this flag is disabled, pressing SHIFT at boot time will disable
;all other MSX-DOS ROMs but not this one.
;When enabled, all other MSX-DOS ROMs will be disabled except if
;GRAPH is pressed at boot time.
DISABLE_OTHERS_BY_DEFAULT: equ 0

;Kept from the original BIOS, where it selected the second set of CH376 ports
;to allow two interfaces in the same computer. It must be 0 on the PicoVerse
;(it also renames the CALL commands to USBRESET2, USBMENU2... when set).
USE_ALTERNATIVE_PORTS: equ 0

;Implement the "panic button":
;pressing CAPS+ESC will abort the current USB operation
;and reset the device
IMPLEMENT_PANIC_BUTTON: equ 1

; -----------------------------------------------------------------------------
; Debugging switches
; You shouldn't enable these unless you are, well, debugging
; -----------------------------------------------------------------------------

;Enable to debug DSKIO calls: whenever DSKIO is called, text mode is enabled,
;the input parameters are printed, and system stops waiting for a key press
DEBUG_DSKIO: equ 0

;Enable to wait for a key press after displaying the device information
;at boot time
WAIT_KEY_ON_INIT: equ 0

;Run a self test of the floppy drive at boot time (TEST UNIT READY and READ CAPACITY)
;and print its results, with the transport diagnostics if a command fails.
;Meant for troubleshooting new drives, it adds a few lines to the boot messages.
BOOT_SELFTEST: equ 0

;The simulation of a fake storage device that the CH376 version had
;isn't available, the RP2040 firmware does the file system access
USE_FAKE_STORAGE_DEVICE: equ 0


; -----------------------------------------------------------------------------
; ROM configuration
; -----------------------------------------------------------------------------

;The address to switch the ROM bank in the Konami SCC mapper emulated by the
;PicoVerse. The mapper has four 8K banks: this address selects the one at
;4000h-5FFFh and 7000h selects the one at 6000h-7FFFh
ROM_BANK_SWITCH: equ 5000h

;Not used on the PicoVerse, the Konami SCC mapper is emulated.
USE_ASCII8_ROM_MAPPER: equ 0

;The ROM banks where all the code lives.
;Note that these refer to 16K banks, made of two consecutive 8K mapper banks.
ROM_BANK_0: equ 0
ROM_BANK_1: equ 1
