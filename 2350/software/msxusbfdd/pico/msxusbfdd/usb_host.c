// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// usb_host.c - USB host side of the PicoVerse USB FDD firmware (runs on core 1)
//
// See usb_host.h
//
// The UFI transport follows the "USB Mass Storage Class Control/Bulk/Interrupt (CBI) Transport"
// and "Bulk-Only Transport" specifications. The command handling mimics what the Z80 code of the
// original BIOS did: when a command fails (stall or non-zero status) REQUEST SENSE is issued and
// its ASC/ASCQ are returned to the MSX.
//
// The Pico SDK host controller driver can't abort a transfer and panics on clear stall requests,
// so the data toggles are reset by sending CLEAR_FEATURE(ENDPOINT_HALT) to every endpoint of the
// drive and then reopening the endpoints in the controller driver.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "hardware/structs/usb.h"
#include "hardware/structs/usb_dpram.h"
#include "hardware/irq.h"
#include "tusb.h"
#include "host/usbh_pvt.h"
#include "host/hcd.h"
#include "portable/raspberrypi/rp2040/rp2040_usb.h"

#include "mailbox.h"
#include "fs_service.h"
#include "usb_host.h"

// ---------------------------------------------------------------------------
// Timeouts
// ---------------------------------------------------------------------------
#define TIMEOUT_NORMAL_MS       10000   // per transfer of a UFI command (Windows retries a failed transfer after 5 s)
#define TIMEOUT_LONG_MS         120000  // for operations like FORMAT UNIT
#define RESET_NO_DEVICE_MS      800     // RESET: time to wait for a device to show up
#define RESET_SETTLE_MS         4000    // RESET: time to wait for devices behind a hub, or for unsupported ones
#define RESET_EVENT_SETTLE_MS   2500    // RESET: time without enumeration events to consider it complete
#define RESET_MAX_MS            12000   // RESET: absolute maximum
#define FS_SYNC_IDLE_MS         300     // flush the open file after this time without commands
#define INT_PROBE_MS            3000    // wait for the interrupt status of a CBI drive until it proves to work

// Last transfer result codes that are not a xfer_result_t (diagnostics)
#define DIAG_NOT_COMPLETED      0xE0

// Stages of a command (diagnostics), 10h is added while the sense data is requested
#define STG_ADSC                1       // CB/CBI: command block on the control endpoint
#define STG_DATA                2       // data stage on the bulk endpoints
#define STG_STATUS              3       // CBI: status on the interrupt endpoint
#define STG_CBW                 5       // Bulk-Only: command block wrapper
#define STG_CSW                 7       // Bulk-Only: command status wrapper
#define STG_SENSE               0x10
#define STG_RECOVERY            0x20    // clear halts, reset

// Time to wait after the enumeration of a floppy drive before the first command: many drives are
// still initializing (and lose or wedge the commands they receive meanwhile). Linux waits 1 s.
#define UFI_SETTLE_MS           500

#define USB_REQ_TYPE_CLASS_INTERFACE_OUT    0x21
#define USB_REQ_TYPE_STANDARD_ENDPOINT_OUT  0x02

// ---------------------------------------------------------------------------
// State of the connected device
// ---------------------------------------------------------------------------
static uint8_t  cur_type = PV_DEV_NONE;     // primary device type
static uint8_t  cur_addr = 0;
static uint16_t cur_vid = 0, cur_pid = 0;
static bool     cur_hub = false;
static bool     ev_connect = false;         // events since the last PV_CMD_DEV_CHANGE
static bool     ev_disconnect = false;
static absolute_time_t last_event;

static bool     long_timeout = false;

static inline uint32_t cmd_timeout_ms(void)
{
    return long_timeout ? TIMEOUT_LONG_MS : TIMEOUT_NORMAL_MS;
}

// A new device becomes the primary one. A FDD or a storage device replaces
// an "other" device, but a second FDD or storage device is ignored.
static void primary_set(uint8_t type, uint8_t daddr)
{
    last_event = get_absolute_time();

    if (cur_type != PV_DEV_NONE && !(cur_type == PV_DEV_OTHER && type != PV_DEV_OTHER))
    {
        return;
    }

    cur_type = type;
    cur_addr = daddr;
    cur_vid = cur_pid = 0;
    tuh_vid_pid_get(daddr, &cur_vid, &cur_pid);

    hcd_devtree_info_t info;
    hcd_devtree_get_info(daddr, &info);
    cur_hub = info.hub_addr != 0;

    ev_connect = true;
}

static void primary_clear(uint8_t daddr)
{
    last_event = get_absolute_time();

    if (cur_type != PV_DEV_NONE && cur_addr == daddr)
    {
        cur_type = PV_DEV_NONE;
        cur_addr = 0;
        ev_disconnect = true;
    }
}

// ---------------------------------------------------------------------------
// UFI floppy drive: application class driver
// ---------------------------------------------------------------------------
typedef struct {
    bool     in_use;                // the class driver opened a device
    bool     mounted;               // enumeration of the interface completed
    uint8_t  daddr;
    uint8_t  itf;
    uint8_t  protocol;              // MSC_PROTOCOL_CBI, MSC_PROTOCOL_CBI_NO_INTERRUPT or MSC_PROTOCOL_BOT
    bool     has_int;
    bool     int_ok;                // the interrupt endpoint reported the status of a command at least once
    uint8_t  int_timeouts;          // commands without interrupt status, while int_ok is false
    bool     media_changed;         // disk was just inserted or drive has not read a sector yet
    uint8_t  ep_in, ep_out, ep_int;
    uint8_t  pid_in, pid_out, pid_int; // DATA0/DATA1 toggle of the bulk and interrupt endpoints on epx
    tusb_desc_endpoint_t desc_in, desc_out, desc_int;

    // Diagnostics for the BIOS (PV_CMD_DEV_INFO): the first transfer of a command that did not succeed
    uint8_t  stage;                 // stage of the command being executed, see STG_*
    bool     fail_valid;
    uint8_t  fail_stage;
    uint8_t  fail_ep;               // endpoint address, 0 for the control endpoint
    uint8_t  fail_xres;             // xfer_result_t of the transfer or DIAG_NOT_COMPLETED
    uint8_t  fail_err;              // mailbox result code of the transfer
    uint8_t  fail_len;              // bytes transferred
    uint8_t  fail_sie;              // SIE_STATUS bits 31-24: data seq error, ack, stall, nak, rx timeout, overflow, bit stuff, crc
    uint8_t  max_wait;              // slowest transfer (completed or timed out) in 100 ms units, saturated at 255
    uint8_t  fail_count;            // commands that failed with a USB error since the drive was connected

    volatile bool        xfer_done;
    volatile xfer_result_t xfer_result;
    volatile uint32_t    xfer_len;

    uint32_t tag;
    uint8_t  cdb[12]   __attribute__((aligned(4)));
    uint8_t  intbuf[8] __attribute__((aligned(4)));
    uint8_t  sense[18] __attribute__((aligned(4)));
    msc_cbw_t cbw      __attribute__((aligned(4)));
    msc_csw_t csw     __attribute__((aligned(4)));
} ufi_dev_t;

static ufi_dev_t ufi;

static bool ufi_init(void)
{
    memset(&ufi, 0, sizeof(ufi));
    return true;
}

static bool ufi_deinit(void)
{
    return true;
}

static bool ufi_open(uint8_t rhport, uint8_t daddr, tusb_desc_interface_t const *itf, uint16_t max_len)
{
    (void)rhport;

    if (itf->bInterfaceClass != TUSB_CLASS_MSC || itf->bInterfaceSubClass != MSC_SUBCLASS_UFI)
    {
        return false;   // not a floppy drive, maybe the MSC driver wants it
    }

    if (itf->bInterfaceProtocol != MSC_PROTOCOL_CBI &&
        itf->bInterfaceProtocol != MSC_PROTOCOL_CBI_NO_INTERRUPT &&
        itf->bInterfaceProtocol != MSC_PROTOCOL_BOT)
    {
        return false;
    }

    if (ufi.in_use)
    {
        return false;   // only one floppy drive is supported
    }

    uint16_t drv_len = (uint16_t)(sizeof(tusb_desc_interface_t) + itf->bNumEndpoints * sizeof(tusb_desc_endpoint_t));
    TU_ASSERT(drv_len <= max_len);

    ufi_init();
    ufi.protocol = itf->bInterfaceProtocol;
    ufi.itf = itf->bInterfaceNumber;

    tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const *)tu_desc_next(itf);
    for (uint8_t i = 0; i < itf->bNumEndpoints; i++)
    {
        TU_ASSERT(TUSB_DESC_ENDPOINT == ep->bDescriptorType);

        if (ep->bmAttributes.xfer == TUSB_XFER_BULK)
        {
            if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN)
            {
                ufi.ep_in = ep->bEndpointAddress;
                ufi.desc_in = *ep;
            }
            else
            {
                ufi.ep_out = ep->bEndpointAddress;
                ufi.desc_out = *ep;
            }
            // Bulk endpoints are driven on epx (see ufi_bulk_epx), not opened as periodic INT_EPs
        }
        else if (ep->bmAttributes.xfer == TUSB_XFER_INTERRUPT && tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN)
        {
            ufi.ep_int = ep->bEndpointAddress;
            ufi.desc_int = *ep;
            if (ufi.desc_int.bInterval > 10)
            {
                // Some drives (TEAC UF000x) declare 127 ms, which makes the RP2040 wait 127 ms
                // between polls whenever the first IN token after the data stage is NAKed
                ufi.desc_int.bInterval = 10;
            }
            ufi.has_int = true;
            TU_ASSERT(tuh_edpt_open(daddr, &ufi.desc_int));
        }

        ep = (tusb_desc_endpoint_t const *)tu_desc_next(ep);
    }

    TU_ASSERT(ufi.ep_in && ufi.ep_out);

    // The status of the commands needs the interrupt endpoint, without it the drive is a CB device
    if (ufi.protocol == MSC_PROTOCOL_CBI && !ufi.has_int)
    {
        ufi.protocol = MSC_PROTOCOL_CBI_NO_INTERRUPT;
    }

    ufi.media_changed = true;
    ufi.daddr = daddr;
    ufi.in_use = true;
    return true;
}

// Drives learned to have no working interrupt endpoint, remembered while the firmware runs
static bool     learned_cb = false;
static uint16_t learned_cb_vid, learned_cb_pid;
static absolute_time_t ufi_mounted_at;

// The device is ready: the BIOS can use it
static void ufi_ready(uint8_t daddr, uint8_t itf_num)
{
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(daddr, &vid, &pid);
    if (ufi.protocol == MSC_PROTOCOL_CBI && learned_cb && vid == learned_cb_vid && pid == learned_cb_pid)
    {
        ufi.protocol = MSC_PROTOCOL_CBI_NO_INTERRUPT;
    }

    ufi_mounted_at = get_absolute_time();
    ufi.mounted = true;
    primary_set(PV_DEV_FDD, daddr);

    usbh_driver_set_config_complete(daddr, itf_num);
}

static void set_interface_complete(tuh_xfer_t *xfer)
{
    // Drives that have a single setting may stall the request: it doesn't matter
    if (ufi.in_use && ufi.daddr == xfer->daddr)
    {
        ufi_ready(xfer->daddr, ufi.itf);
    }
}

static bool ufi_set_config(uint8_t daddr, uint8_t itf_num)
{
    TU_ASSERT(ufi.in_use && ufi.daddr == daddr && ufi.itf == itf_num);

    // Windows sends SET_INTERFACE (alternate setting 0) right after SET_CONFIGURATION and some drives
    // (TEAC UF000x) seem to need it, so it's done too.
    tusb_control_request_t const request = {
        .bmRequestType_bit = {
            .recipient = TUSB_REQ_RCPT_INTERFACE,
            .type      = TUSB_REQ_TYPE_STANDARD,
            .direction = TUSB_DIR_OUT
        },
        .bRequest = TUSB_REQ_SET_INTERFACE,
        .wValue   = 0,
        .wIndex   = itf_num,
        .wLength  = 0
    };
    tuh_xfer_t xfer = {
        .daddr       = daddr,
        .ep_addr     = 0,
        .setup       = &request,
        .buffer      = NULL,
        .complete_cb = set_interface_complete,
        .user_data   = 0
    };

    if (!tuh_control_xfer(&xfer))
    {
        ufi_ready(daddr, itf_num);
    }
    return true;
}

static bool ufi_xfer_cb(uint8_t daddr, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
    (void)daddr;
    (void)ep_addr;

    ufi.xfer_result = result;
    ufi.xfer_len = xferred_bytes;
    ufi.xfer_done = true;
    return true;
}

static void ufi_close(uint8_t daddr)
{
    if (ufi.in_use && ufi.daddr == daddr)
    {
        ufi.mounted = false;
        ufi.in_use = false;
        primary_clear(daddr);
    }
}

static usbh_class_driver_t const ufi_driver = {
    .name       = "UFI",
    .init       = ufi_init,
    .deinit     = ufi_deinit,
    .open       = ufi_open,
    .set_config = ufi_set_config,
    .xfer_cb    = ufi_xfer_cb,
    .close      = ufi_close
};

// Called by TinyUSB to get the application class drivers
usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = 1;
    return &ufi_driver;
}

// ---------------------------------------------------------------------------
// UFI transport helpers (blocking, they run TinyUSB while waiting)
// ---------------------------------------------------------------------------
typedef enum { WAIT_OK = 0, WAIT_TIMEOUT, WAIT_ABORT, WAIT_GONE } wait_status_t;

static uint8_t wait_error(wait_status_t w)
{
    switch (w)
    {
        case WAIT_TIMEOUT: return PV_RES_TIMEOUT;
        case WAIT_ABORT:   return PV_RES_PANIC;
        case WAIT_GONE:    return PV_RES_NO_DEVICE;
        default:           return PV_RES_OK;
    }
}

static struct hw_endpoint *rp2040_ep_pool = NULL;
static uint8_t             dev_ep0_mps[16];
static volatile bool       epx_was_active_on_reset = false;

void __real_hw_endpoint_reset_transfer(struct hw_endpoint *ep);
bool __real_hcd_edpt_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_endpoint_t const *ep_desc);
bool __real_hcd_setup_send(uint8_t rhport, uint8_t dev_addr, uint8_t const setup_packet[8]);
bool __real_hcd_edpt_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr, uint8_t *buffer, uint16_t buflen);
void __real_hcd_event_handler(hcd_event_t const *event, bool in_isr);

#define EPX_SIE_BASE (USB_SIE_CTRL_SOF_EN_BITS | USB_SIE_CTRL_KEEP_ALIVE_EN_BITS | \
                      USB_SIE_CTRL_PULLDOWN_EN_BITS | USB_SIE_CTRL_EP0_INT_1BUF_BITS)

// Wait >= 12 cycles of the 48 MHz USB clock (~250 ns = ~60 CPU cycles at 230 MHz)
static inline void usb_clk_delay(void)
{
    busy_wait_at_least_cycles(64);
}

// Clear RP2350 per-endpoint TX/RX transaction and sequence error counters (which otherwise halt
// the endpoint after 3 RX_TIMEOUTs during floppy spindle motor spin-up)
static inline void clear_rp2350_ep_errors(void)
{
#ifdef USB_SIE_STATUS_ENDPOINT_ERROR_BITS
    usb_hw->ep_rx_error = 0xFFFFFFFFu;
    usb_hw->ep_tx_error = 0xFFFFFFFFu;
    usb_hw_clear->sie_status = USB_SIE_STATUS_ENDPOINT_ERROR_BITS;
#endif
}

static wait_status_t wait_for(volatile bool *done, uint32_t timeout_ms)
{
    absolute_time_t end = make_timeout_time_ms(timeout_ms);

    while (!*done)
    {
#ifdef USB_SIE_STATUS_ENDPOINT_ERROR_BITS
        if ((usb_hw->sie_status & USB_SIE_STATUS_ENDPOINT_ERROR_BITS) ||
            usb_hw->ep_rx_error != 0 || usb_hw->ep_tx_error != 0)
        {
            clear_rp2350_ep_errors();
            if (rp2040_ep_pool)
            {
                for (int i = 1; i <= USB_HOST_INTERRUPT_ENDPOINTS; i++)
                {
                    struct hw_endpoint *ep = &rp2040_ep_pool[i];
                    if (ep->configured && ep->active && (*ep->buffer_control & USB_BUF_CTRL_AVAIL))
                    {
                        uint32_t mask = 1u << (ep->interrupt_num + 1);
                        uint32_t bc = *ep->buffer_control;
                        usb_hw_clear->int_ep_ctrl = mask;
                        usb_clk_delay();
                        *ep->buffer_control = bc & ~USB_BUF_CTRL_AVAIL;
                        usb_clk_delay();
                        *ep->buffer_control = bc;
                        usb_clk_delay();
                        usb_hw_set->int_ep_ctrl = mask;
                    }
                }
            }
        }
#endif
        tuh_task();
        if (*done)
        {
            break;
        }
        if (pv_mbox.abort)
        {
            return WAIT_ABORT;
        }
        if (!ufi.mounted)
        {
            return WAIT_GONE;
        }
        if (time_reached(end))
        {
            return WAIT_TIMEOUT;
        }
    }
    return WAIT_OK;
}

void __wrap_hw_endpoint_reset_transfer(struct hw_endpoint *ep)
{
    if (!rp2040_ep_pool)
    {
        if (ep->endpoint_control == &usbh_dpram->epx_ctrl)
        {
            rp2040_ep_pool = ep;
        }
        else
        {
            rp2040_ep_pool = ep - (ep->interrupt_num + 1);
        }
    }
    if (ep == rp2040_ep_pool)
    {
        epx_was_active_on_reset = ep->active;
    }
    __real_hw_endpoint_reset_transfer(ep);
}

bool __wrap_hcd_edpt_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_endpoint_t const *ep_desc)
{
    last_event = get_absolute_time();
    if (tu_edpt_number(ep_desc->bEndpointAddress) == 0 && dev_addr < TU_ARRAY_SIZE(dev_ep0_mps))
    {
        dev_ep0_mps[dev_addr] = (uint8_t)tu_edpt_packet_size(ep_desc);
    }
    return __real_hcd_edpt_open(rhport, dev_addr, ep_desc);
}

static inline void restore_ep0_mps(uint8_t dev_addr)
{
    if (rp2040_ep_pool && dev_addr < TU_ARRAY_SIZE(dev_ep0_mps) && dev_ep0_mps[dev_addr])
    {
        rp2040_ep_pool[0].wMaxPacketSize = dev_ep0_mps[dev_addr];
    }
}

bool __wrap_hcd_setup_send(uint8_t rhport, uint8_t dev_addr, uint8_t const setup_packet[8])
{
    clear_rp2350_ep_errors();
    restore_ep0_mps(dev_addr);
    return __real_hcd_setup_send(rhport, dev_addr, setup_packet);
}

bool __wrap_hcd_edpt_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr, uint8_t *buffer, uint16_t buflen)
{
    clear_rp2350_ep_errors();
    if (tu_edpt_number(ep_addr) == 0)
    {
        restore_ep0_mps(dev_addr);
    }
    return __real_hcd_edpt_xfer(rhport, dev_addr, ep_addr, buffer, buflen);
}

void __wrap_hcd_event_handler(hcd_event_t const *event, bool in_isr)
{
    if (event->event_id == HCD_EVENT_XFER_COMPLETE &&
        tu_edpt_number(event->xfer_complete.ep_addr) == 0 &&
        event->xfer_complete.result == XFER_RESULT_STALLED)
    {
        if (epx_was_active_on_reset)
        {
            usbh_dpram->epx_buf_ctrl = 0;
            __real_hcd_event_handler(event, in_isr);
            return;
        }

        // epx was not active: the STALL came from an active bulk or interrupt endpoint in ep_pool[1..15]
        if (rp2040_ep_pool)
        {
            struct hw_endpoint *stalled_ep = NULL;

            for (int i = 1; i <= USB_HOST_INTERRUPT_ENDPOINTS; i++)
            {
                struct hw_endpoint *ep = &rp2040_ep_pool[i];
                if (ep->configured && ep->active && (*ep->buffer_control & USB_BUF_CTRL_STALL))
                {
                    stalled_ep = ep;
                    break;
                }
            }
            if (!stalled_ep)
            {
                for (int i = 1; i <= USB_HOST_INTERRUPT_ENDPOINTS; i++)
                {
                    struct hw_endpoint *ep = &rp2040_ep_pool[i];
                    if (ep->configured && ep->active && ep->dev_addr <= CFG_TUH_DEVICE_MAX)
                    {
                        stalled_ep = ep;
                        break;
                    }
                }
            }
            if (stalled_ep)
            {
                hcd_event_t ev = *event;
                ev.dev_addr = stalled_ep->dev_addr;
                ev.xfer_complete.ep_addr = stalled_ep->ep_addr;
                ev.xfer_complete.len = stalled_ep->xferred_len;
                *stalled_ep->buffer_control = 0;
                usb_hw_clear->buf_status = 3u << ((stalled_ep->interrupt_num + 1) * 2);
                __real_hw_endpoint_reset_transfer(stalled_ep);
                __real_hcd_event_handler(&ev, in_isr);
            }
        }
        return;
    }

    __real_hcd_event_handler(event, in_isr);
}

bool __wrap_hcd_edpt_abort_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr)
{
    (void)rhport;
    if (!rp2040_ep_pool)
    {
        return false;
    }

    if (tu_edpt_number(ep_addr) == 0)
    {
        uint32_t save = save_and_disable_interrupts();
        usb_hw->sie_ctrl = USB_SIE_CTRL_SOF_EN_BITS | USB_SIE_CTRL_KEEP_ALIVE_EN_BITS |
                           USB_SIE_CTRL_PULLDOWN_EN_BITS | USB_SIE_CTRL_EP0_INT_1BUF_BITS |
                           USB_SIE_CTRL_STOP_TRANS_BITS;
        busy_wait_at_least_cycles(24);
        usbh_dpram->epx_buf_ctrl = 0;
        usb_hw_clear->buf_status = 0x03u;
        usb_hw_clear->sie_status = USB_SIE_STATUS_TRANS_COMPLETE_BITS | USB_SIE_STATUS_STALL_REC_BITS;
        __real_hw_endpoint_reset_transfer(&rp2040_ep_pool[0]);
        restore_interrupts(save);
        return true;
    }

    for (int i = 1; i <= USB_HOST_INTERRUPT_ENDPOINTS; i++)
    {
        struct hw_endpoint *ep = &rp2040_ep_pool[i];
        if (ep->configured && ep->dev_addr == dev_addr && ep->ep_addr == ep_addr)
        {
            uint32_t save = save_and_disable_interrupts();
            uint32_t mask = 1u << (ep->interrupt_num + 1);
            usb_hw_clear->int_ep_ctrl = mask;
            busy_wait_at_least_cycles(24);
            if (*ep->buffer_control & USB_BUF_CTRL_AVAIL)
            {
                // Buffer was never consumed on the bus: undo the next_pid flip done by prepare_ep_buffer()
                ep->next_pid = (*ep->buffer_control & USB_BUF_CTRL_DATA1_PID) ? 1u : 0u;
            }
            *ep->buffer_control = 0;
            usb_hw_clear->buf_status = 3u << ((ep->interrupt_num + 1) * 2);
            __real_hw_endpoint_reset_transfer(ep);
            usb_hw_set->int_ep_ctrl = mask;
            restore_interrupts(save);
            return true;
        }
    }
    return false;
}

void __wrap_hcd_port_reset(uint8_t rhport)
{
    (void)rhport;
    hw_clear_bits(&usb_hw->inte, USB_INTE_HOST_CONN_DIS_BITS);
    hw_set_bits(&usb_hw->sie_ctrl, USB_SIE_CTRL_RESET_BUS_BITS);
}

void __wrap_hcd_port_reset_end(uint8_t rhport)
{
    (void)rhport;
    absolute_time_t end = make_timeout_time_ms(20);
    while (!(usb_hw->sie_status & USB_SIE_STATUS_SPEED_BITS) && !time_reached(end))
    {
        tight_loop_contents();
    }
    usb_hw_clear->sie_status = USB_SIE_STATUS_SPEED_BITS;
    hw_set_bits(&usb_hw->inte, USB_INTE_HOST_CONN_DIS_BITS);
}

// ---------------------------------------------------------------------------
// Hardware level diagnostics
//
// The TinyUSB host driver for the RP2040 clears the controller error flags itself (an RX timeout is
// swallowed and the transfer is retried by the hardware), so what happens on the bus is invisible.
// A shared interrupt handler counts the events, and the hardware endpoint registers are
// saved when a transfer times out (before the recovery clears them), for the BIOS to show.
// ---------------------------------------------------------------------------
static volatile uint8_t hw_cnt[6];  // RX timeouts, data sequence errors, stalls, transfers complete, buffers complete, connect/disconnect

static inline void hw_count(int i)
{
    if (hw_cnt[i] < 255)
    {
        hw_cnt[i]++;
    }
}

// Runs before the TinyUSB handler (which clears the flags): it is added later with the same
// priority and the handler added last runs first. It only counts.
static void hw_diag_irq(void)
{
    uint32_t ints = usb_hw->ints;

    if (ints & USB_INTS_ERROR_RX_TIMEOUT_BITS)  hw_count(0);
    if (ints & USB_INTS_ERROR_DATA_SEQ_BITS)    hw_count(1);
    if (ints & USB_INTS_STALL_BITS)             hw_count(2);
    if (ints & USB_INTS_TRANS_COMPLETE_BITS)    hw_count(3);
    if (ints & USB_INTS_BUFF_STATUS_BITS)       hw_count(4);
    if (ints & USB_INTS_HOST_CONN_DIS_BITS)     hw_count(5);

    clear_rp2350_ep_errors();
}

typedef struct {
    bool     valid;
    uint8_t  cnt[6];
    uint32_t int_ep_ctrl;       // enabled interrupt (and bulk) endpoints
    uint32_t sie_ctrl;
    uint32_t sie_status;
    uint32_t epx_buf;           // buffer control of the control endpoint
    uint32_t addr[6];           // address and endpoint polled by the first hardware endpoints
    uint32_t buf[6];            // their buffer control (bit 10 = available, bit 15 = full...)
} hw_snap_t;

static hw_snap_t hw_snap;

static void hw_snapshot(void)
{
    if (hw_snap.valid)
    {
        return;
    }

    hw_snap.valid = true;
    for (int i = 0; i < 6; i++)
    {
        hw_snap.cnt[i] = hw_cnt[i];
        hw_snap.addr[i] = usb_hw->int_ep_addr_ctrl[i];
        hw_snap.buf[i] = usbh_dpram->int_ep_buffer_ctrl[i].ctrl;
    }
    hw_snap.int_ep_ctrl = usb_hw->int_ep_ctrl;
    hw_snap.sie_ctrl = usb_hw->sie_ctrl;
    hw_snap.sie_status = usb_hw->sie_status;
    hw_snap.epx_buf = usbh_dpram->epx_buf_ctrl;
}

static volatile bool        ctl_done;
static volatile xfer_result_t ctl_result;

static void ctl_complete_cb(tuh_xfer_t *xfer)
{
    ctl_result = xfer->result;
    ctl_done = true;
}

// Control transfer to the floppy drive. Returns a mailbox result code (PV_RES_*) and
// the transfer result (stalled, failed...) in *xres when the transfer completed.
static uint8_t ufi_control_impl(uint8_t req_type, uint8_t request, uint16_t value, uint16_t index,
                                void *buf, uint16_t len, xfer_result_t *xres)
{
    tusb_control_request_t setup = {
        .bmRequestType = req_type,
        .bRequest = request,
        .wValue = value,
        .wIndex = index,
        .wLength = len
    };
    tuh_xfer_t xfer = {
        .daddr = ufi.daddr,
        .ep_addr = 0,
        .setup = &setup,
        .buffer = (uint8_t *)buf,
        .complete_cb = ctl_complete_cb,
        .user_data = 0
    };

    absolute_time_t end = make_timeout_time_ms(cmd_timeout_ms());

    while (!tuh_control_xfer(&xfer))
    {
        // The control pipe may be busy, for example if a hub is enumerating another device
        // or a previous transfer that timed out is still pending
        tuh_task();
        if (pv_mbox.abort)
        {
            return PV_RES_PANIC;
        }
        if (!ufi.mounted)
        {
            return PV_RES_NO_DEVICE;
        }
        if (time_reached(end))
        {
            return PV_RES_TIMEOUT;
        }
    }

    // The completion callback only runs inside tuh_task(), so clearing the flag here can't
    // lose the completion of this transfer, but it discards a late one of a previous transfer
    ctl_done = false;

    wait_status_t w = wait_for(&ctl_done, cmd_timeout_ms());
    if (w != WAIT_OK)
    {
        hw_snapshot();
        tuh_edpt_abort_xfer(ufi.daddr, 0);
        return wait_error(w);
    }

    *xres = ctl_result;
    return PV_RES_OK;
}

// Remember the first transfer of the command that did not succeed, for the BIOS diagnostics
static void diag_note(uint8_t ep, uint8_t r, xfer_result_t x, uint32_t len)
{
    if (ufi.fail_valid || (r == PV_RES_OK && x == XFER_RESULT_SUCCESS))
    {
        return;
    }

    uint32_t sie = usb_hw->sie_status;

    ufi.fail_valid = true;
    ufi.fail_stage = ufi.stage;
    ufi.fail_ep = ep;
    ufi.fail_err = r;
    ufi.fail_xres = (r == PV_RES_OK) ? (uint8_t)x : DIAG_NOT_COMPLETED;
    ufi.fail_len = (uint8_t)len;
    ufi.fail_sie = (uint8_t)(sie >> 24);
}

// Keep the duration of the slowest transfer
static void diag_time(absolute_time_t start)
{
    int64_t units = absolute_time_diff_us(start, get_absolute_time()) / 100000;

    if (units > 255)
    {
        units = 255;
    }
    if (units > ufi.max_wait)
    {
        ufi.max_wait = (uint8_t)units;
    }
}

static uint8_t ufi_control(uint8_t req_type, uint8_t request, uint16_t value, uint16_t index,
                           void *buf, uint16_t len, xfer_result_t *xres)
{
    xfer_result_t x = XFER_RESULT_INVALID;
    absolute_time_t t0 = get_absolute_time();
    uint8_t r = ufi_control_impl(req_type, request, value, index, buf, len, &x);

    diag_time(t0);
    diag_note(0, r, x, len);
    *xres = x;
    return r;
}

static uint8_t stage_flags = 0;     // STG_SENSE while the sense data is requested

#define SET_STAGE(s)    (ufi.stage = (uint8_t)((s) | stage_flags))

// Reset the host-side data toggle of a single endpoint to DATA0
static void ufi_reset_ep_toggle(uint8_t ep_addr)
{
    if (ep_addr == ufi.ep_in)
    {
        ufi.pid_in = 0;
        return;
    }
    if (ep_addr == ufi.ep_out)
    {
        ufi.pid_out = 0;
        return;
    }
    if (ep_addr == ufi.ep_int)
    {
        ufi.pid_int = 0;
        return;
    }
    if (!rp2040_ep_pool)
    {
        return;
    }
    for (int i = 1; i <= USB_HOST_INTERRUPT_ENDPOINTS; i++)
    {
        struct hw_endpoint *ep = &rp2040_ep_pool[i];
        if (ep->configured && ep->dev_addr == ufi.daddr && ep->ep_addr == ep_addr)
        {
            *ep->buffer_control = 0;
            __real_hw_endpoint_reset_transfer(ep);
            ep->next_pid = 0;
            break;
        }
    }
}

// Clear the halt condition and data toggle of a single endpoint, in the device and in the host
static void ufi_clear_ep_halt(uint8_t ep_addr)
{
    xfer_result_t xres;
    uint8_t saved_stage = ufi.stage;

    ufi.stage = STG_RECOVERY;
    ufi_control(USB_REQ_TYPE_STANDARD_ENDPOINT_OUT, TUSB_REQ_CLEAR_FEATURE, TUSB_REQ_FEATURE_EDPT_HALT,
                ep_addr, NULL, 0, &xres);
    ufi_reset_ep_toggle(ep_addr);
    ufi.stage = saved_stage;
}

// Clear the halt condition and the data toggle of the bulk endpoints, in the device and in the host
static void ufi_clear_halts(void)
{
    if (!ufi.mounted)
    {
        return;
    }
    ufi_clear_ep_halt(ufi.ep_in);
    ufi_clear_ep_halt(ufi.ep_out);
}

#ifdef USB_SIE_STATUS_ENDPOINT_ERROR_BITS
#define SIE_ERR_CLEAR_BITS (USB_SIE_STATUS_TRANS_COMPLETE_BITS | \
                            USB_SIE_STATUS_STALL_REC_BITS | \
                            USB_SIE_STATUS_DATA_SEQ_ERROR_BITS | \
                            USB_SIE_STATUS_RX_TIMEOUT_BITS | \
                            USB_SIE_STATUS_RX_OVERFLOW_BITS | \
                            USB_SIE_STATUS_BIT_STUFF_ERROR_BITS | \
                            USB_SIE_STATUS_CRC_ERROR_BITS | \
                            USB_SIE_STATUS_ENDPOINT_ERROR_BITS)
#else
#define SIE_ERR_CLEAR_BITS (USB_SIE_STATUS_TRANS_COMPLETE_BITS | \
                            USB_SIE_STATUS_STALL_REC_BITS | \
                            USB_SIE_STATUS_DATA_SEQ_ERROR_BITS | \
                            USB_SIE_STATUS_RX_TIMEOUT_BITS | \
                            USB_SIE_STATUS_RX_OVERFLOW_BITS | \
                            USB_SIE_STATUS_BIT_STUFF_ERROR_BITS | \
                            USB_SIE_STATUS_CRC_ERROR_BITS)
#endif

static inline void epx_stop_trans(void)
{
    usb_hw->sie_ctrl = EPX_SIE_BASE | USB_SIE_CTRL_STOP_TRANS_BITS;
    usb_clk_delay();
    usbh_dpram->epx_buf_ctrl = 0;
    clear_rp2350_ep_errors();
    usb_hw_clear->buf_status = 0x03u;
    usb_hw_clear->sie_status = SIE_ERR_CLEAR_BITS | USB_SIE_STATUS_NAK_REC_BITS;
}

// Execute a Bulk IN or Bulk OUT transfer directly on the RP2040/RP2350's main
// asynchronous endpoint engine (epx) rather than TinyUSB's periodic INT_EP pool.
//
// Why this is critical for USB floppy drives like the TEAC UF000x:
// 1. TinyUSB's hcd_rp2040.c places Bulk endpoints into the periodic INT_EP slots, which the
//    SIE hardware only polls once per 1.000 ms SOF frame and never retries within the same frame on
//    NAK. A 3.5" HD floppy drive streams data off the spinning magnetic disk at 500 kbps (64 bytes
//    every 1.024 ms); whenever the 1.024 ms disk byte stream drifts across the 1.000 ms SOF poll, the
//    drive replies NAK and INT_EP waits a full 1.000 ms before polling again, overflowing the drive's
//    FDC FIFO and triggering a Track-0 recalibrate/seek grinding loop.
// 2. On RP2350, the USB controller added per-endpoint 3-strike transaction error counters
//    (usb_hw->ep_rx_error / ep_tx_error and USB_SIE_STATUS_ENDPOINT_ERROR_BITS). When the UF000x
//    turns on its spindle motor from standby (LED off), it briefly stops answering IN tokens
//    (causing > 3 RX_TIMEOUTs), which trips EP0_TRANSACTION / ENDPOINT_ERROR on RP2350 and stops epx
//    polling unless ep_rx_error / ep_tx_error are cleared and epx is kept running.
static uint8_t __no_inline_not_in_flash_func(ufi_bulk_epx)(uint8_t ep_addr, void *buf, uint16_t len,
                                                           uint32_t *xferred, xfer_result_t *xres,
                                                           uint32_t timeout_ms)
{
    uint8_t *ptr = (uint8_t *)buf;
    uint16_t remaining = len;
    uint32_t total = 0;
    bool out = (tu_edpt_dir(ep_addr) == TUSB_DIR_OUT);
    uint8_t ep_num = tu_edpt_number(ep_addr);
    uint8_t *pid = (ep_addr == ufi.ep_out) ? &ufi.pid_out
                 : (ep_addr == ufi.ep_int) ? &ufi.pid_int
                                           : &ufi.pid_in;
    uint16_t mps = (ep_addr == ufi.ep_out) ? tu_edpt_packet_size(&ufi.desc_out)
                 : (ep_addr == ufi.ep_int) ? tu_edpt_packet_size(&ufi.desc_int)
                                           : tu_edpt_packet_size(&ufi.desc_in);
    if (mps == 0 || mps > 64)
    {
        mps = 64;
    }

    bool need_pre = (hcd_port_speed_get(0) != tuh_speed_get(ufi.daddr));
    absolute_time_t end = make_timeout_time_ms(timeout_ms);
    uint8_t res = PV_RES_OK;

    // Suspend periodic INT_EP polling and USBCTRL_IRQ while epx is driven directly
    uint32_t saved_int_ep = usb_hw->int_ep_ctrl;
    uint32_t saved_nak_poll = usb_hw->nak_poll;
    usb_hw->int_ep_ctrl = 0;
    irq_set_enabled(USBCTRL_IRQ, false);

    while (remaining > 0)
    {
        uint16_t pkt_len = (remaining > mps) ? mps : remaining;

        // Poll every 100 us while waiting for the first packet (motor spin-up / head seek),
        // then switch to 16 us once the FDC is actively streaming packets off the disk
        usb_hw->nak_poll = (total == 0)
            ? ((100u << USB_NAK_POLL_DELAY_FS_LSB) | (100u << USB_NAK_POLL_DELAY_LS_LSB))
            : 0x00100010u;

        usbh_dpram->epx_buf_ctrl = 0;
        clear_rp2350_ep_errors();
        usb_hw_clear->buf_status = 0x03u;
        usb_hw_clear->sie_status = SIE_ERR_CLEAR_BITS | USB_SIE_STATUS_NAK_REC_BITS;

        if (out)
        {
            memcpy((void *)&usbh_dpram->epx_data[0], ptr, pkt_len);
        }

        usbh_dpram->epx_ctrl = EP_CTRL_ENABLE_BITS |
                               EP_CTRL_INTERRUPT_PER_BUFFER |
                               ((uint32_t)TUSB_XFER_BULK << EP_CTRL_BUFFER_TYPE_LSB) |
                               0x180u;

        uint32_t bc = (uint32_t)pkt_len |
                      (*pid ? USB_BUF_CTRL_DATA1_PID : USB_BUF_CTRL_DATA0_PID) |
                      USB_BUF_CTRL_LAST |
                      USB_BUF_CTRL_SEL |
                      (out ? USB_BUF_CTRL_FULL : 0u);
        usbh_dpram->epx_buf_ctrl = bc;
        usb_clk_delay();
        usbh_dpram->epx_buf_ctrl = bc | USB_BUF_CTRL_AVAIL;
        usb_clk_delay();

        usb_hw->dev_addr_ctrl = (uint32_t)ufi.daddr | ((uint32_t)ep_num << USB_ADDR_ENDP_ENDPOINT_LSB);

        uint32_t flags = EPX_SIE_BASE |
                         (out ? USB_SIE_CTRL_SEND_DATA_BITS : USB_SIE_CTRL_RECEIVE_DATA_BITS) |
                         (need_pre ? USB_SIE_CTRL_PREAMBLE_EN_BITS : 0u);
        usb_hw->sie_ctrl = flags;
        usb_clk_delay();
        usb_hw->sie_ctrl = flags | USB_SIE_CTRL_START_TRANS_BITS;

        uint32_t last_poll_us = time_us_32();

        while (true)
        {
            uint32_t sie = usb_hw->sie_status;
            uint32_t bfs = usb_hw->buf_status;

            if ((bfs & 0x03u) && (sie & USB_SIE_STATUS_TRANS_COMPLETE_BITS))
            {
                usb_clk_delay();
                uint32_t done_bc = usbh_dpram->epx_buf_ctrl;
                clear_rp2350_ep_errors();
                usb_hw_clear->buf_status = 0x03u;
                usb_hw_clear->sie_status = USB_SIE_STATUS_TRANS_COMPLETE_BITS | USB_SIE_STATUS_NAK_REC_BITS;
                usbh_dpram->epx_buf_ctrl = 0;
                hw_count(3);
                hw_count(4);

                uint16_t got = (uint16_t)(done_bc & USB_BUF_CTRL_LEN_MASK);
                if (!out)
                {
                    if (got > pkt_len)
                    {
                        got = pkt_len;
                    }
                    memcpy(ptr, (const void *)&usbh_dpram->epx_data[0], got);
                }
                else
                {
                    got = pkt_len;
                }

                // Match the ~2 us Cortex-M0+ copy time so the Full-Speed ACK handshake (1.58 us)
                // finishes on the wire before starting the next packet
                busy_wait_us_32(2);

                *pid ^= 1u;
                ptr += got;
                total += got;
                remaining = (uint16_t)(remaining - got);
                if (remaining == 0 || got < mps)
                {
                    *xres = XFER_RESULT_SUCCESS;
                    goto done;
                }
                break;
            }
            if (sie & USB_SIE_STATUS_STALL_REC_BITS)
            {
                hw_count(2);
                epx_stop_trans();
                *xres = XFER_RESULT_STALLED;
                goto done;
            }
            if (sie & USB_SIE_STATUS_NAK_REC_BITS)
            {
                usb_hw_clear->sie_status = USB_SIE_STATUS_NAK_REC_BITS;
                last_poll_us = time_us_32();
            }

            bool restart = false;
            if (sie & USB_SIE_STATUS_RX_TIMEOUT_BITS)
            {
                clear_rp2350_ep_errors();
                usb_hw_clear->sie_status = USB_SIE_STATUS_RX_TIMEOUT_BITS;
                hw_count(0);
            }
            if (sie & (USB_SIE_STATUS_CRC_ERROR_BITS |
                       USB_SIE_STATUS_BIT_STUFF_ERROR_BITS |
                       USB_SIE_STATUS_RX_OVERFLOW_BITS))
            {
                clear_rp2350_ep_errors();
                usb_hw_clear->sie_status = USB_SIE_STATUS_CRC_ERROR_BITS |
                                           USB_SIE_STATUS_BIT_STUFF_ERROR_BITS |
                                           USB_SIE_STATUS_RX_OVERFLOW_BITS;
            }
            if (sie & USB_SIE_STATUS_DATA_SEQ_ERROR_BITS)
            {
                clear_rp2350_ep_errors();
                usb_hw_clear->sie_status = USB_SIE_STATUS_DATA_SEQ_ERROR_BITS |
                                           USB_SIE_STATUS_TRANS_COMPLETE_BITS;
                hw_count(1);
                restart = true;
            }
#ifdef USB_SIE_STATUS_ENDPOINT_ERROR_BITS
            if ((sie & USB_SIE_STATUS_ENDPOINT_ERROR_BITS) ||
                usb_hw->ep_rx_error != 0 || usb_hw->ep_tx_error != 0)
            {
                clear_rp2350_ep_errors();
                restart = true;
            }
#endif
            if (restart || (uint32_t)(time_us_32() - last_poll_us) > 300u)
            {
                if (!(usb_hw->buf_status & 0x03u) &&
                    !(usb_hw->sie_status & USB_SIE_STATUS_STALL_REC_BITS))
                {
                    epx_stop_trans();
                    if (out)
                    {
                        memcpy((void *)&usbh_dpram->epx_data[0], ptr, pkt_len);
                    }
                    usbh_dpram->epx_ctrl = EP_CTRL_ENABLE_BITS |
                                           EP_CTRL_INTERRUPT_PER_BUFFER |
                                           ((uint32_t)TUSB_XFER_BULK << EP_CTRL_BUFFER_TYPE_LSB) |
                                           0x180u;
                    usbh_dpram->epx_buf_ctrl = bc;
                    usb_clk_delay();
                    usbh_dpram->epx_buf_ctrl = bc | USB_BUF_CTRL_AVAIL;
                    usb_clk_delay();
                    usb_hw->dev_addr_ctrl = (uint32_t)ufi.daddr | ((uint32_t)ep_num << USB_ADDR_ENDP_ENDPOINT_LSB);
                    usb_hw->sie_ctrl = flags;
                    usb_clk_delay();
                    usb_hw->sie_ctrl = flags | USB_SIE_CTRL_START_TRANS_BITS;
                }
                last_poll_us = time_us_32();
            }

            if (pv_mbox.abort)
            {
                epx_stop_trans();
                res = PV_RES_PANIC;
                goto done;
            }
            if (!ufi.mounted || !(sie & USB_SIE_STATUS_SPEED_BITS))
            {
                epx_stop_trans();
                res = PV_RES_NO_DEVICE;
                goto done;
            }
            if (time_reached(end))
            {
                hw_snapshot();
                epx_stop_trans();
                res = PV_RES_TIMEOUT;
                goto done;
            }
        }
    }

    *xres = XFER_RESULT_SUCCESS;

done:
    if (rp2040_ep_pool)
    {
        __real_hw_endpoint_reset_transfer(&rp2040_ep_pool[0]);
    }
    usb_hw->nak_poll = saved_nak_poll;
    clear_rp2350_ep_errors();
    usb_hw_clear->buf_status = 0x03u;
    usb_hw_clear->sie_status = SIE_ERR_CLEAR_BITS | USB_SIE_STATUS_NAK_REC_BITS;
    usb_hw->int_ep_ctrl = saved_int_ep;
    irq_set_enabled(USBCTRL_IRQ, true);
    *xferred = total;
    return res;
}

// Transfer on a bulk or interrupt endpoint. Returns a mailbox result code and, when the
// transfer completed (even with a stall), the amount of data and the transfer result.
static uint8_t ufi_bulk_impl(uint8_t ep, void *buf, uint16_t len, uint32_t *xferred, xfer_result_t *xres,
                             uint32_t timeout_ms)
{
    if (ep == ufi.ep_in || ep == ufi.ep_out)
    {
        return ufi_bulk_epx(ep, buf, len, xferred, xres, timeout_ms);
    }

    ufi.xfer_done = false;

    if (!usbh_edpt_claim(ufi.daddr, ep))
    {
        return PV_RES_UNEXPECTED;
    }
    if (!usbh_edpt_xfer(ufi.daddr, ep, (uint8_t *)buf, len))
    {
        usbh_edpt_release(ufi.daddr, ep);
        return PV_RES_UNEXPECTED;
    }

    wait_status_t w = wait_for(&ufi.xfer_done, timeout_ms);
    if (w != WAIT_OK)
    {
        // The transfer is still pending in the controller: abort it
        hw_snapshot();
        tuh_edpt_abort_xfer(ufi.daddr, ep);
        return wait_error(w);
    }

    *xferred = ufi.xfer_len;
    *xres = ufi.xfer_result;
    return PV_RES_OK;
}

static uint8_t ufi_bulk_timeout(uint8_t ep, void *buf, uint16_t len, uint32_t *xferred, xfer_result_t *xres,
                                uint32_t timeout_ms)
{
    xfer_result_t x = XFER_RESULT_INVALID;
    uint32_t got = 0;
    absolute_time_t t0 = get_absolute_time();
    uint8_t r = ufi_bulk_impl(ep, buf, len, &got, &x, timeout_ms);

    diag_time(t0);
    diag_note(ep, r, x, got);
    *xferred = got;
    *xres = x;
    return r;
}

static uint8_t ufi_bulk(uint8_t ep, void *buf, uint16_t len, uint32_t *xferred, xfer_result_t *xres)
{
    return ufi_bulk_timeout(ep, buf, len, xferred, xres, cmd_timeout_ms());
}

// Data stage of a command on the bulk endpoints.
// Sets *stalled if the endpoint stalled. Returns a mailbox result code.
static uint8_t ufi_data_stage(uint8_t *data, uint16_t len, bool out, uint16_t *xferred, bool *stalled)
{
    uint32_t n = 0;
    xfer_result_t xres = XFER_RESULT_INVALID;

    *xferred = 0;
    if (!len)
    {
        return PV_RES_OK;
    }

    uint8_t r = ufi_bulk(out ? ufi.ep_out : ufi.ep_in, data, len, &n, &xres);
    if (r != PV_RES_OK)
    {
        return r;
    }

    *xferred = (uint16_t)n;
    if (xres == XFER_RESULT_STALLED)
    {
        *stalled = true;
        return PV_RES_OK;
    }
    return xres == XFER_RESULT_SUCCESS ? PV_RES_OK : PV_RES_DATA_ERROR;
}

// Control/Bulk/Interrupt transport of a UFI command.
// *failed is set if the command failed (the sense data has to be retrieved).
static uint8_t ufi_transport_cbi(const uint8_t *cdb, uint8_t *data, uint16_t len, bool out,
                                 uint16_t *xferred, bool *failed)
{
    xfer_result_t xres = XFER_RESULT_INVALID;
    uint32_t n = 0;
    bool stalled = false;
    uint8_t r;

    // Command block: class specific ADSC request on the control endpoint
    memcpy(ufi.cdb, cdb, 12);
    SET_STAGE(STG_ADSC);
    r = ufi_control(USB_REQ_TYPE_CLASS_INTERFACE_OUT, 0, 0, ufi.itf, ufi.cdb, 12, &xres);
    if (r != PV_RES_OK)
    {
        return r;
    }
    if (xres == XFER_RESULT_STALLED)
    {
        *failed = true;     // the command was not accepted
        return PV_RES_OK;
    }
    if (xres != XFER_RESULT_SUCCESS)
    {
        return PV_RES_DATA_ERROR;
    }

    // Data stage
    SET_STAGE(STG_DATA);
    r = ufi_data_stage(data, len, out, xferred, &stalled);
    if (r != PV_RES_OK)
    {
        return r;
    }
    if (stalled)
    {
        *failed = true;
        ufi_clear_ep_halt(out ? ufi.ep_out : ufi.ep_in);
    }

    // Status stage: two bytes on the interrupt endpoint (ASC and ASCQ), non-zero means failure.
    // Even if the data stage stalled, the drive may have queued the 2-byte completion interrupt on
    // ep_int, which must be drained so it is not read by the following REQUEST SENSE.
    SET_STAGE(STG_STATUS);
    if (ufi.protocol == MSC_PROTOCOL_CBI)
    {
        // Some drives declare an interrupt endpoint that never reports anything. Until it proves
        // to work the wait is short, and a drive that fails twice is handled as a CB drive.
        uint32_t wait_ms = stalled ? 500 : (ufi.int_ok ? cmd_timeout_ms() : INT_PROBE_MS);

        memset(ufi.intbuf, 0, sizeof(ufi.intbuf));
        r = ufi_bulk_timeout(ufi.ep_int, ufi.intbuf, 2, &n, &xres, wait_ms);
        if (r == PV_RES_TIMEOUT)
        {
            *failed = true;     // no status received, the sense data will tell

            if (!stalled && !ufi.int_ok && ++ufi.int_timeouts >= 2)
            {
                ufi.protocol = MSC_PROTOCOL_CBI_NO_INTERRUPT;

                uint16_t vid = 0, pid = 0;
                tuh_vid_pid_get(ufi.daddr, &vid, &pid);
                learned_cb = true;
                learned_cb_vid = vid;
                learned_cb_pid = pid;
            }
        }
        else if (r != PV_RES_OK)
        {
            return r;
        }
        else if (xres == XFER_RESULT_STALLED)
        {
            *failed = true;
            ufi_clear_ep_halt(ufi.ep_int);
        }
        else
        {
            ufi.int_ok = true;
            ufi.int_timeouts = 0;

            if (xres != XFER_RESULT_SUCCESS || n < 2 || ufi.intbuf[0] || ufi.intbuf[1])
            {
                *failed = true;

                // The bulk IN endpoint can be stalled after the last byte of the data stage when
                // the status reports an error (CBI specification): clear it like the CH376 BIOS did
                if (len > 0 && !out && !stalled && !(stage_flags & STG_SENSE))
                {
                    ufi_clear_ep_halt(ufi.ep_in);
                }
            }
        }
    }
    else if (!stalled && (len == 0 || out || *xferred < len))
    {
        // CB drive: there is no status stage. The commands that move no data, the ones that
        // write and the ones that returned less than requested are checked with REQUEST SENSE.
        *failed = true;
    }

    return PV_RES_OK;
}

// Bulk-Only transport recovery: reset the interface and clear the halt condition of the endpoints
static void ufi_bot_reset_recovery(void)
{
    xfer_result_t xres;

    ufi_control(USB_REQ_TYPE_CLASS_INTERFACE_OUT, MSC_REQ_RESET, 0, ufi.itf, NULL, 0, &xres);
    ufi_clear_halts();
}

// Bulk-Only transport of a UFI command
static uint8_t ufi_transport_bot(const uint8_t *cdb, uint8_t *data, uint16_t len, bool out,
                                 uint16_t *xferred, bool *failed)
{
    xfer_result_t xres = XFER_RESULT_INVALID;
    uint32_t n = 0;
    bool stalled = false;
    uint8_t r;

    memset(&ufi.cbw, 0, sizeof(ufi.cbw));
    ufi.cbw.signature = MSC_CBW_SIGNATURE;
    ufi.cbw.tag = ++ufi.tag;
    ufi.cbw.total_bytes = len;
    ufi.cbw.dir = (len && !out) ? TUSB_DIR_IN_MASK : 0;
    ufi.cbw.lun = 0;
    ufi.cbw.cmd_len = 12;
    memcpy(ufi.cbw.command, cdb, 12);

    SET_STAGE(STG_CBW);
    r = ufi_bulk(ufi.ep_out, &ufi.cbw, sizeof(ufi.cbw), &n, &xres);
    if (r != PV_RES_OK)
    {
        return r;
    }
    if (xres != XFER_RESULT_SUCCESS)
    {
        ufi_bot_reset_recovery();
        return PV_RES_DATA_ERROR;
    }

    SET_STAGE(STG_DATA);
    r = ufi_data_stage(data, len, out, xferred, &stalled);
    if (r != PV_RES_OK)
    {
        if (r == PV_RES_DATA_ERROR)
        {
            ufi_bot_reset_recovery();
        }
        return r;
    }
    if (stalled)
    {
        ufi_clear_ep_halt(out ? ufi.ep_out : ufi.ep_in);
    }

    // Status stage: the command status wrapper, read again once if the endpoint stalled
    SET_STAGE(STG_CSW);
    for (int attempt = 0; attempt < 2; attempt++)
    {
        memset(&ufi.csw, 0, sizeof(ufi.csw));
        r = ufi_bulk(ufi.ep_in, &ufi.csw, sizeof(ufi.csw), &n, &xres);
        if (r != PV_RES_OK)
        {
            return r;
        }
        if (xres == XFER_RESULT_STALLED && attempt == 0)
        {
            ufi_clear_ep_halt(ufi.ep_in);
            continue;
        }
        break;
    }

    if (xres != XFER_RESULT_SUCCESS || n != sizeof(ufi.csw) ||
        ufi.csw.signature != MSC_CSW_SIGNATURE || ufi.csw.tag != ufi.cbw.tag)
    {
        ufi_bot_reset_recovery();
        return PV_RES_DATA_ERROR;
    }

    if (ufi.csw.status == MSC_CSW_STATUS_PHASE_ERROR)
    {
        ufi_bot_reset_recovery();
        return PV_RES_DATA_ERROR;
    }

    if (ufi.csw.status == MSC_CSW_STATUS_FAILED || stalled)
    {
        *failed = true;
    }
    return PV_RES_OK;
}

static uint8_t ufi_transport(const uint8_t *cdb, uint8_t *data, uint16_t len, bool out,
                             uint16_t *xferred, bool *failed)
{
    *failed = false;
    *xferred = 0;

    if (ufi.protocol == MSC_PROTOCOL_BOT)
    {
        return ufi_transport_bot(cdb, data, len, out, xferred, failed);
    }
    return ufi_transport_cbi(cdb, data, len, out, xferred, failed);
}

// Execute a UFI command and, if it fails, get the sense data
// Returns a mailbox result code, *asc and *ascq are the sense codes of a failed command.
static uint8_t ufi_execute(const uint8_t *cdb, uint8_t *data, uint16_t len, bool out,
                           uint16_t *xferred, uint8_t *asc, uint8_t *ascq)
{
    static const uint8_t request_sense_cdb[12] = { 0x03, 0, 0, 0, 18, 0, 0, 0, 0, 0, 0, 0 };
    bool failed = false;

    *asc = 0;
    *ascq = 0;

    uint8_t r = ufi_transport(cdb, data, len, out, xferred, &failed);
    if (r != PV_RES_OK)
    {
        return r;
    }

    // The status of REQUEST SENSE itself is ignored: it returns the sense data of the previous command
    if (!failed || cdb[0] == 0x03)
    {
        return PV_RES_OK;
    }

    uint16_t sense_len = 0;
    bool sense_failed = false;
    memset(ufi.sense, 0, sizeof(ufi.sense));
    stage_flags = STG_SENSE;
    r = ufi_transport(request_sense_cdb, ufi.sense, sizeof(ufi.sense), false, &sense_len, &sense_failed);
    stage_flags = 0;
    if (r != PV_RES_OK)
    {
        return r;
    }

    if (sense_len < 14)
    {
        // The command failed and the reason is unknown: report an internal target failure,
        // which the BIOS turns into a generic I/O error, instead of success
        *asc = 0x44;
        *ascq = 0;
        return PV_RES_OK;
    }

    *asc = ufi.sense[12];
    *ascq = ufi.sense[13];
    return PV_RES_OK;
}

static bool ufi_delay_ms(uint32_t ms)
{
    absolute_time_t end = make_timeout_time_ms(ms);
    while (!time_reached(end))
    {
        tuh_task();
        if (pv_mbox.abort || !ufi.mounted)
        {
            return false;
        }
    }
    return true;
}

static uint8_t cmd_ufi_exec(void)
{
    if (cur_type != PV_DEV_FDD || !ufi.mounted)
    {
        return PV_RES_NO_DEVICE;
    }

    uint16_t len = (uint16_t)(pv_mbox.arg[12] | (pv_mbox.arg[13] << 8));
    bool out = (pv_mbox.arg[14] & 1) != 0;
    uint8_t cdb[12];
    uint16_t xferred = 0;
    uint8_t asc = 0, ascq = 0;

    if (len > PV_BUF_SIZE)
    {
        return PV_RES_DATA_ERROR;
    }

    memcpy(cdb, pv_mbox.arg, 12);

    // Let the drive finish its own initialization after the enumeration
    while (absolute_time_diff_us(ufi_mounted_at, get_absolute_time()) < (int64_t)UFI_SETTLE_MS * 1000)
    {
        tuh_task();
        if (pv_mbox.abort)
        {
            return PV_RES_PANIC;
        }
        if (!ufi.mounted)
        {
            return PV_RES_NO_DEVICE;
        }
    }

    ufi.fail_valid = false;     // the diagnostics keep the first failure of this command
    hw_snap.valid = false;
    memset((void *)hw_cnt, 0, sizeof(hw_cnt));
    stage_flags = 0;

    uint8_t r = ufi_execute(cdb, pv_mbox.buf, len, out, &xferred, &asc, &ascq);
    if (r != PV_RES_OK)
    {
        if (ufi.fail_count < 255)
        {
            ufi.fail_count++;
        }
        return r;
    }

    // Pace BIOS spin-up / unit-attention retries so the drive is not hammered in a tight loop
    if (asc == 0x04 || asc == 0x28 || asc == 0x29)
    {
        ufi_delay_ms(100);
    }

    pv_mbox.resp[0] = (uint8_t)(xferred & 0xFF);
    pv_mbox.resp[1] = (uint8_t)(xferred >> 8);
    pv_mbox.resp[2] = asc;
    pv_mbox.resp[3] = ascq;
    return PV_RES_OK;
}

static uint8_t cmd_bus_reset(void)
{
    if (cur_type != PV_DEV_FDD || !ufi.mounted)
    {
        return PV_RES_OK;   // nothing to recover
    }

    if (ufi.protocol == MSC_PROTOCOL_BOT)
    {
        ufi_bot_reset_recovery();
    }
    else
    {
        ufi_clear_halts();
    }
    return PV_RES_OK;
}

// ---------------------------------------------------------------------------
// USB mass storage (flash drives): TinyUSB MSC driver, used through FatFs
// ---------------------------------------------------------------------------
static uint8_t  msc_addr = 0;
static bool     msc_ready = false;
static scsi_inquiry_resp_t inquiry_resp;

bool usb_msc_ready(void)
{
    return msc_ready;
}

uint8_t usb_msc_addr(void)
{
    return msc_addr;
}

void usb_msc_fault(void)
{
    msc_ready = false;
    fs_service_invalidate();
    if (msc_addr)
    {
        primary_clear(msc_addr);
    }
}

bool fs_dev_present(void)
{
    return msc_ready;
}

bool fs_dev_name(char *name28)
{
    memcpy(name28, inquiry_resp.vendor_id, 8);
    memcpy(name28 + 8, inquiry_resp.product_id, 16);
    memcpy(name28 + 24, inquiry_resp.product_rev, 4);
    return true;
}

static bool inquiry_complete_cb(uint8_t dev_addr, tuh_msc_complete_data_t const *cb_data)
{
    (void)cb_data;

    // Only 512 bytes sectors are supported by the FatFs configuration
    if (tuh_msc_get_block_size(dev_addr, 0) == 512)
    {
        msc_ready = true;
        primary_set(PV_DEV_STORAGE, dev_addr);
    }
    else
    {
        primary_set(PV_DEV_OTHER, dev_addr);
    }
    return true;
}

void tuh_msc_mount_cb(uint8_t dev_addr)
{
    if (msc_addr != 0)
    {
        return;     // only one storage device is supported
    }

    msc_addr = dev_addr;
    msc_ready = false;
    memset(&inquiry_resp, ' ', sizeof(inquiry_resp));
    tuh_msc_inquiry(dev_addr, 0, &inquiry_resp, inquiry_complete_cb, 0);
}

void tuh_msc_umount_cb(uint8_t dev_addr)
{
    if (dev_addr != msc_addr)
    {
        return;
    }

    msc_addr = 0;
    msc_ready = false;
    fs_service_invalidate();
    primary_clear(dev_addr);
}

// ---------------------------------------------------------------------------
// Device detection
// ---------------------------------------------------------------------------
static tusb_desc_device_t other_desc;

// Called when the enumeration of any device (not hubs) is complete
void tuh_mount_cb(uint8_t daddr)
{
    last_event = get_absolute_time();

    if ((ufi.mounted && ufi.daddr == daddr) || msc_addr == daddr)
    {
        return;     // already handled by the class driver
    }

    // Anything else is "another device"
    if (tuh_descriptor_get_device_sync(daddr, &other_desc, sizeof(other_desc)) == XFER_RESULT_SUCCESS &&
        other_desc.bDeviceClass != TUSB_CLASS_HUB)
    {
        primary_set(PV_DEV_OTHER, daddr);
    }
}

void tuh_umount_cb(uint8_t daddr)
{
    primary_clear(daddr);
}

// ---------------------------------------------------------------------------
// Device commands
// ---------------------------------------------------------------------------
// True if any device, including hubs, finished its enumeration
static bool any_device_mounted(void)
{
    for (uint8_t daddr = 1; daddr <= CFG_TUH_DEVICE_MAX + CFG_TUH_HUB; daddr++)
    {
        if (tuh_mounted(daddr))
        {
            return true;
        }
    }
    return false;
}

static uint8_t cmd_reset(void)
{
    absolute_time_t start = get_absolute_time();

    // Wait for the RP2040 to enumerate the device that is connected, if any
    while (true)
    {
        tuh_task();

        if (pv_mbox.abort)
        {
            return PV_RES_PANIC;
        }

        absolute_time_t now = get_absolute_time();
        int64_t since_start = absolute_time_diff_us(start, now) / 1000;
        int64_t since_event = absolute_time_diff_us(last_event, now) / 1000;
        bool connected = hcd_port_connect_status(0);

        if ((cur_type == PV_DEV_FDD || cur_type == PV_DEV_STORAGE) &&
            (!cur_hub || since_event > 700))
        {
            break;
        }

        if (!connected && since_start > RESET_NO_DEVICE_MS)
        {
            break;
        }
        if (connected && any_device_mounted() && since_event > RESET_EVENT_SETTLE_MS && since_start > RESET_SETTLE_MS)
        {
            break;      // a hub without devices, or a device that is not a floppy or storage
        }
        if (since_start > RESET_MAX_MS)
        {
            break;
        }
    }

    ev_connect = false;
    ev_disconnect = false;
    pv_mbox.resp[0] = (cur_type != PV_DEV_NONE) ? 1 : 0xFF;
    return PV_RES_OK;
}

static uint8_t cmd_dev_change(void)
{
    if (ev_connect)
    {
        pv_mbox.resp[0] = 1;
    }
    else if (ev_disconnect)
    {
        pv_mbox.resp[0] = 0xFF;
    }
    else
    {
        pv_mbox.resp[0] = 0;
    }

    ev_connect = false;
    ev_disconnect = false;
    return PV_RES_OK;
}

static uint8_t cmd_dev_info(void)
{
    pv_mbox.resp[0] = cur_type;
    pv_mbox.resp[1] = (uint8_t)(cur_vid & 0xFF);
    pv_mbox.resp[2] = (uint8_t)(cur_vid >> 8);
    pv_mbox.resp[3] = (uint8_t)(cur_pid & 0xFF);
    pv_mbox.resp[4] = (uint8_t)(cur_pid >> 8);
    pv_mbox.resp[5] = cur_hub ? 1 : 0;

    // Diagnostics of the floppy drive transport, shown by the BIOS when a command fails: protocol,
    // flags, and the stage, endpoint, result, USB error, length and SIE status of the first transfer
    // of the last command that did not succeed
    if (cur_type == PV_DEV_FDD && ufi.in_use)
    {
        pv_mbox.resp[6] = ufi.protocol;
        pv_mbox.resp[7] = (ufi.has_int ? 1 : 0) | (ufi.int_ok ? 2 : 0);
        pv_mbox.resp[8] = ufi.fail_valid ? ufi.fail_stage : 0xFF;
        pv_mbox.resp[9] = ufi.fail_ep;
        pv_mbox.resp[10] = ufi.fail_xres;
        pv_mbox.resp[11] = ufi.fail_err;
        pv_mbox.resp[12] = ufi.fail_len;
        pv_mbox.resp[13] = ufi.fail_sie;
        pv_mbox.resp[14] = ufi.max_wait;
        pv_mbox.resp[15] = ufi.fail_count;

        // Hardware diagnostics in the data buffer (see hw_snap_t): 6 counters, 2 unused bytes and
        // 16 little endian words: int_ep_ctrl, sie_ctrl, sie_status, epx_buf, addr[6], buf[6]
        uint8_t *p = pv_mbox.buf;
        uint32_t w[16];
        const uint8_t *cnt = hw_snap.valid ? hw_snap.cnt : (const uint8_t *)hw_cnt;

        memset(w, 0, sizeof(w));
        if (hw_snap.valid)
        {
            w[0] = hw_snap.int_ep_ctrl;
            w[1] = hw_snap.sie_ctrl;
            w[2] = hw_snap.sie_status;
            w[3] = hw_snap.epx_buf;
            memcpy(&w[4], hw_snap.addr, sizeof(hw_snap.addr));
            memcpy(&w[10], hw_snap.buf, sizeof(hw_snap.buf));
        }
        memset(p, 0, 72);
        memcpy(p, cnt, 6);
        memcpy(p + 8, w, sizeof(w));
    }
    return PV_RES_OK;
}

static void execute_command(uint8_t cmd)
{
    uint8_t result;

    memset(pv_mbox.resp, 0, sizeof(pv_mbox.resp));

    switch (cmd)
    {
        case PV_CMD_RESET:
            result = cmd_reset();
            break;
        case PV_CMD_DEV_CHANGE:
            result = cmd_dev_change();
            break;
        case PV_CMD_DEV_INFO:
            result = cmd_dev_info();
            break;
        case PV_CMD_BUS_RESET:
            result = cmd_bus_reset();
            break;
        case PV_CMD_LONG_TIMEOUT:
            long_timeout = (pv_mbox.arg[0] & 1) != 0;
            result = PV_RES_OK;
            break;
        case PV_CMD_UFI_EXEC:
            result = cmd_ufi_exec();
            break;
        default:
            if (cmd >= PV_CMD_FS_MOUNT && cmd <= PV_CMD_FS_ATTR)
            {
                result = fs_service_command(cmd, &pv_mbox);
            }
            else
            {
                result = PV_RES_UNEXPECTED;
            }
            break;
    }

    pv_mbox.result = result;
    __dmb();
    pv_mbox.status = 0;     // not busy, the MSX can read the result
}

// ---------------------------------------------------------------------------
// Core 1 entry point
// ---------------------------------------------------------------------------
void usb_host_task(void)
{
    last_event = get_absolute_time();
    absolute_time_t last_cmd = get_absolute_time();

    tuh_init(0);

    // Added after TinyUSB's handler with the same priority, so it runs first
    irq_add_shared_handler(USBCTRL_IRQ, hw_diag_irq, PICO_SHARED_IRQ_HANDLER_HIGHEST_ORDER_PRIORITY);

    // The RP2040 host driver calls panic() on a data toggle error, which would hang this core for good
    // and leave the MSX waiting for the mailbox forever: without the interrupt the transfer just
    // fails or times out.
    hw_clear_bits(&usb_hw->inte, USB_INTE_ERROR_DATA_SEQ_BITS);

    while (true)
    {
        tuh_task();

        uint8_t cmd = pv_mbox.cmd;
        if (cmd)
        {
            pv_mbox.cmd = 0;
            __dmb();
            execute_command(cmd);
            last_cmd = get_absolute_time();
        }
        else if (fs_service_dirty() &&
                 absolute_time_diff_us(last_cmd, get_absolute_time()) > (FS_SYNC_IDLE_MS * 1000))
        {
            fs_service_sync();
        }
    }
}
