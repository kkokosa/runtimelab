param(
    [int]$MaxTries = 15,
    [int]$Duration = 120,
    [int]$GcThreads = 1,
    [switch]$ConcFinishFullTrace
)

$ErrorActionPreference = 'Continue'
$cdb = (Get-ChildItem "C:\Program Files\WindowsApps\Microsoft.WinDbg_*\amd64\cdb.exe" | Select-Object -First 1).FullName
$exe = "C:\github\runtimelab\src\LXRGC\samples\WebApi\publish\WebApi.exe"
$outDir = "C:\github\runtimelab\src\LXRGC"

# Clean env from any prior state
Get-ChildItem Env: | Where-Object { $_.Name -like 'LXR*' -or $_.Name -eq 'DOTNET_GCName' } | ForEach-Object { Remove-Item "Env:$($_.Name)" -ErrorAction SilentlyContinue }

$env:DOTNET_GCName = "LXRGC.dll"
$env:LXR_GC_THREADS = "$GcThreads"
$env:LXR_CONCURRENT = "1"
$env:LXR_EVAC = "1"
$env:LXR_REMSET = "1"
$env:LXR_FAULT_DIAG = "1"
$env:LXRGC_BENCH_DURATION_SECONDS = "$Duration"
if ($ConcFinishFullTrace) { $env:LXR_CONC_FINISH_FULLTRACE = "1" }

# On 2nd-chance AV: dump phase, faulting context+stack, all thread stacks, then kill.
$dbgCmds = '.symfix; .reload; sxd -c2 "da /c 60 poi(LXRGC!g_lxrPhase); r; kb 80; ~*kb 16; .kill; qd" av; g; qd'

for ($i = 1; $i -le $MaxTries; $i++) {
    Write-Output "=== AV try $i / $MaxTries (gcThreads=$GcThreads) ==="
    $log = Join-Path $outDir "av-out-$i.log"
    & $cdb -c $dbgCmds $exe *>&1 | Tee-Object -FilePath $log | Out-Null
    # Real crash = unhandled managed AccessViolationException / Fatal error, NOT a
    # benign first-chance managed null-check. A clean run prints ##RESULT##.
    $hit = Select-String -Path $log -Pattern 'Fatal error|AccessViolationException|protected memory' -Quiet
    $ok  = Select-String -Path $log -Pattern '##RESULT##' -Quiet
    if ($hit) {
        Write-Output "  AV CAPTURED on try $i -> $log"
        Copy-Item $log (Join-Path $outDir "av-captured.log") -Force
        break
    } else {
        if ($ok) { Write-Output "  try $i clean (##RESULT## ok)" }
        else     { Write-Output "  try $i no-crash-but-no-result" }
        Remove-Item $log -ErrorAction SilentlyContinue
    }
}
Write-Output "DONE"
