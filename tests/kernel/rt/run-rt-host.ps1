param(
    [ValidateSet("0", "4", "8", "12")]
    [string]$Bg = "4",
    [int]$Reps = 3,
    [int]$Timeout = 120,
    [switch]$Deterministic,
    [switch]$Budget,
    [switch]$OneShot,
    [int]$WorkUs = 1800,
    [string]$Out = ""
)

$ErrorActionPreference = "Stop"

# RT-1 宿主隔离：Windows 默认定时器分辨率会放大 QEMU 的 10ms PIT 抖动。
# 0.5ms 是 Windows 可接受的高分辨率档位；进程退出前恢复。
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class AevumNtTimer {
    [DllImport("ntdll.dll")]
    public static extern int NtSetTimerResolution(uint desired, bool set, out uint current);
}
"@

$current = 0
$set = [AevumNtTimer]::NtSetTimerResolution(5000, $true, [ref]$current)
if ($set -ne 0) {
    Write-Output "warning: NtSetTimerResolution failed rc=$set"
}

try {
    $icount = if ($Deterministic) { "1" } else { "0" }
    $budgetArg = if ($Budget) { "1" } else { "0" }
    $oneshotArg = if ($OneShot) { "1" } else { "0" }
    $outArg = if ($Out) { " $Out" } else { "" }
    $cmd = "cd /mnt/d/Aevum && RT_BG_TASKS=$Bg REPS=$Reps TIMEOUT=$Timeout RT_STRICT=1 RT_ICOUNT=$icount RT_BUDGET=$budgetArg RT_BUDGET_ONESHOT=$oneshotArg RT_WORK_US=$WorkUs tests/kernel/rt/run-rt.sh$outArg"
    & "C:\Windows\System32\wsl.exe" -d kali-linux -- bash -lc $cmd
    $code = $LASTEXITCODE
}
finally {
    $ignored = 0
    [AevumNtTimer]::NtSetTimerResolution(5000, $false, [ref]$ignored) | Out-Null
}

exit $code
