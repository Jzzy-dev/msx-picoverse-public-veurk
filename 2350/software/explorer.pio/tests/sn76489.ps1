param([string]$CC = "gcc")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
$firmware = Get-Content (Join-Path $root "pico\explorer\explorer.c") -Raw

function Get-CFunction([string]$name) {
    $match = [regex]::Match($firmware, "(?m)^static[^\r\n]*\b$name\b\)?\s*\([^;{]*\)\s*\{")
    if (!$match.Success) { throw "Cannot find production function $name" }
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

$constants = ([regex]::Matches($firmware,
    '(?m)^#define (?:AUDIO_PROFILE_\w+|DUAL_PSG_WRITE_RING_\w+|PSG_(?:CLOCK|SAMPLE_RATE|VOLUME_SHIFT|PORT_REG|PORT_DATA))\b[^\r\n]*') |
    ForEach-Object { $_.Value }) -join "`n"
$unionStart = $firmware.IndexOf("static union {")
$unionEnd = $firmware.IndexOf("static bool sn_audio_selected", $unionStart)
if ($unionStart -lt 0 -or $unionEnd -le $unionStart) { throw "Missing shared secondary state" }
$code = $firmware.Substring($unionStart, $unionEnd - $unionStart)
foreach ($name in @("sn_audio_selected", "dual_psg_init",
    "dual_psg_write_ring_push", "dual_psg_drain_write_ring", "sn_handle_io_write", "dual_psg_handle_io_write",
    "sn_handle_io_read", "dual_psg_calc_sample", "sn_write_stereo_sample", "dual_psg_write_stereo_sample")) {
    $code += (Get-CFunction $name) + "`n"
}
$pio = Get-Content (Join-Path $root "pico\explorer\msx_bus.pio") -Raw
$counts = @{}
foreach ($program in [regex]::Matches($pio, '(?ms)^\.program (\w+)\s*\r?\n(.*?)(?=^\.program |\z)')) {
    $counts[$program.Groups[1].Value] = @($program.Groups[2].Value -split "`n" |
        Where-Object { $_ -match '^\s*(wait|jmp|set|in|out|push|pull|mov|nop)\b' }).Count
}
foreach ($triple in @(
    @("msx_read_responder", "msx_write_captor", "msx_io_read_responder"),
    @("msx_read_responder", "msx_write_captor", "msx_io_read_wait_responder"))) {
    $total = 0
    foreach ($name in $triple) { $total += $counts[$name] }
    if ($total -gt 32 -or $total -lt 30) { throw "Invalid PIO instruction budget: $triple = $total" }
}
$work = Join-Path $PSScriptRoot (".sn76489-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory $work | Out-Null
try {
    [IO.File]::WriteAllText((Join-Path $work "sn-constants.h"), $constants)
    [IO.File]::WriteAllText((Join-Path $work "sn-production.h"), $code)
    $exe = Join-Path $work "sn76489.exe"
    $ay = Join-Path $work "emu2149.o"
    $audio = Join-Path $root "pico\explorer\audio"
    & $CC -std=c11 -O2 -c (Join-Path $audio "emu2149.c") -o $ay
    if ($LASTEXITCODE -ne 0) { throw "AY baseline compilation failed" }
    & $CC -std=c11 -O2 -Wall -Wextra -Werror -I $work -I $audio `
        (Join-Path $PSScriptRoot "sn76489.c") (Join-Path $audio "sn76489.c") $ay -o $exe
    if ($LASTEXITCODE -ne 0) { throw "SN76489 host compilation failed" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "SN76489 regressions failed" }
    & tclsh (Join-Path $PSScriptRoot "sn76489_openmsx.tcl") `
        (Join-Path $root "openmsx\share\scripts\picoverse2350.tcl") $work
    if ($LASTEXITCODE -ne 0) { throw "SN76489 openMSX protocol regressions failed" }
    Write-Output "PASS: shared state fits existing AY instance; both PIO layouts fit 32 instructions"
} finally {
    foreach ($name in @("sn-constants.h", "sn-production.h", "emu2149.o", "sn76489.exe", "Song.PVC")) {
        $file = Join-Path $work $name
        if (Test-Path $file) { Remove-Item -LiteralPath $file }
    }
    Remove-Item -LiteralPath $work
}
