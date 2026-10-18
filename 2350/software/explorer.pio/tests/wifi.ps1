param([string]$CC = "gcc")
$ErrorActionPreference = "Stop"

$root = Split-Path $PSScriptRoot
$firmware = Get-Content (Join-Path $root "pico\explorer\explorer.c") -Raw

function Get-CFunction([string]$name) {
    $match = [regex]::Match($firmware, "(?m)^static[^\r\n]*\b$name\b\)?\s*\([^;{]*\)\s*\{")
    if (!$match.Success) { throw "Cannot find function $name" }
    $end = $match.Index + $match.Length
    $depth = 1
    while ($depth -gt 0 -and $end -lt $firmware.Length) {
        if ($firmware[$end] -eq '{') { $depth++ }
        if ($firmware[$end] -eq '}') { $depth-- }
        $end++
    }
    if ($depth -ne 0) { throw "Unclosed function $name" }
    return $firmware.Substring($match.Index, $end - $match.Index)
}

$code = @'
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define __not_in_flash_func(name) name
#define WIFI_UART_INSTANCE 1

static uint16_t wifi_rx_count, wifi_tx_count;
static uint32_t wifi_tx_busy_deadline_us;
static bool wifi_rx_underrun, wifi_rx_overflow;
static bool hw_busy, hw_writable = true;
static uint8_t wifi_f2_state = 0xFFu;
static uint8_t rom[0x4000];

static void wifi_uart_init_once(void) {}
static void wifi_service(void) {}
static uint16_t wifi_rx_count_snapshot(void) { return wifi_rx_count; }
static bool wifi_hw_tx_busy(void) { return hw_busy; }
static bool uart_is_writable(int uart) { (void)uart; return hw_writable; }
static uint32_t time_us_32(void) { return 1000u; }
static uint8_t wifi_data_read(void) { return 0x5Au; }
'@

$code += "`n" + (([regex]::Matches($firmware,
    '(?m)^#define WIFI_(?:MEM_\w+|RX_FIFO_SIZE|STATUS_\w+)\s+[^\r\n]+') |
    ForEach-Object { $_.Value }) -join "`n") + "`n"
foreach ($name in @("wifi_status_read", "wifi_handle_mem_read")) {
    $code += (Get-CFunction $name) + "`n"
}
$code += @'

int main(void) {
    unsigned cases = 0;

    assert(WIFI_MEM_DATA_ADDR == 0x7F07u);
    assert((WIFI_STATUS_QUICK_RX | WIFI_STATUS_FREE_BITS) == 0x88u);
    assert(wifi_handle_mem_read(rom, sizeof(rom), 0x7F07u) == 0x88u);

    for (unsigned rx = 0; rx < 3; ++rx)
    for (unsigned tx = 0; tx < 16; ++tx)
    for (unsigned err = 0; err < 4; ++err) {
        wifi_rx_count = rx == 0 ? 0u : rx == 1 ? 1u : WIFI_RX_FIFO_SIZE;
        wifi_tx_count = (tx & 1u) != 0u;
        hw_busy = (tx & 2u) != 0u;
        hw_writable = (tx & 4u) == 0u;
        wifi_tx_busy_deadline_us = (tx & 8u) ? 1012u : 0u;
        wifi_rx_underrun = (err & 1u) != 0u;
        wifi_rx_overflow = (err & 2u) != 0u;

        uint8_t expected = 0x88u;
        if (rx != 0u) expected |= WIFI_STATUS_RX_READY;
        if (rx == 2u) expected |= WIFI_STATUS_RX_FULL;
        if (tx != 0u) expected |= WIFI_STATUS_TX_BUSY;
        if (err != 0u) expected |= WIFI_STATUS_UNDERRUN;

        uint8_t status = wifi_handle_mem_read(rom, sizeof(rom), 0x7F07u);
        assert(status == expected);
        /* SymbOS esptry rejects the adapter before NET_STATE if this fails. */
        assert((status & 0x88u) == 0x88u);
        assert((status & 0x40u) == 0u);
        assert(!wifi_rx_underrun && !wifi_rx_overflow);
        assert(wifi_handle_mem_read(rom, sizeof(rom), 0x7F07u) ==
               (expected & (uint8_t)~WIFI_STATUS_UNDERRUN));
        ++cases;
    }

    assert(wifi_handle_mem_read(rom, sizeof(rom), 0x7F05u) == 0xFFu);
    wifi_f2_state = 0xF1u;
    assert(wifi_handle_mem_read(rom, sizeof(rom), 0x7F05u) == 0xF1u);
    assert(wifi_handle_mem_read(rom, sizeof(rom), 0x7F06u) == 0x5Au);
    rom[0] = 0x41u;
    rom[1] = 0x42u;
    assert(wifi_handle_mem_read(rom, sizeof(rom), 0x4000u) == 0x41u);
    assert(wifi_handle_mem_read(rom, sizeof(rom), 0x4001u) == 0x42u);
    assert(wifi_handle_mem_read(rom, sizeof(rom), 0x3FFFu) == 0xFFu);
    assert(wifi_handle_mem_read(rom, sizeof(rom), 0x8000u) == 0xFFu);
    printf("WiFi status: %u states pass SymbOS detection, flags and latch checks; memio routing passes.\n", cases);
    return 0;
}
'@

$exe = Join-Path ([IO.Path]::GetTempPath()) ("picoverse-wifi-" + [guid]::NewGuid() + ".exe")
try {
    $code | & $CC -std=c11 -Wall -Wextra -Werror -x c -o $exe -
    if ($LASTEXITCODE -ne 0) { throw "WiFi host compilation failed" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "WiFi host regressions failed" }
} finally {
    if (Test-Path -LiteralPath $exe) { Remove-Item -LiteralPath $exe }
}
