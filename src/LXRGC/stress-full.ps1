param(
    [int]$MaxTries = 12,
    [int]$Duration = 120,
    [int]$GcThreads = 16,
    [int]$Concurrent = 0,
    [int]$Evac = 1,
    [int]$VerifyTrace = 0,
    [int]$ReadyToRun = 1,
    [int]$LineReuse = 0
)
Get-ChildItem Env: | Where-Object { ($_.Name -like 'LXR*' -and $_.Name -ne 'LXR_DUMP_ON_GAP') -or $_.Name -eq 'DOTNET_GCName' } | ForEach-Object { Remove-Item "Env:$($_.Name)" -ErrorAction SilentlyContinue }
$exe = "C:\github\runtimelab\src\LXRGC\samples\WebApi\publish\WebApi.exe"
$dir = Split-Path $exe
$env:DOTNET_GCName = "LXRGC.dll"
$env:LXR_GC_THREADS = "$GcThreads"
if ($Concurrent -ne 0) { $env:LXR_CONCURRENT = "1" }
if ($Evac -ne 0) { $env:LXR_EVAC = "1" }
if ($LineReuse -ne 0) { $env:LXR_LINE_REUSE = "1" }
$env:LXR_REMSET = "1"
$env:LXR_FAULT_DIAG = "1"
if ($VerifyTrace -ne 0) { $env:LXR_VERIFY_TRACE = "1" }
$env:DOTNET_ReadyToRun = "$ReadyToRun"
$env:LXRGC_BENCH_DURATION_SECONDS = "$Duration"
$timeoutSec = $Duration + 60
$hang = 0; $av = 0; $ok = 0
for ($i = 1; $i -le $MaxTries; $i++) {
    $log = "C:\github\runtimelab\src\LXRGC\stwfull-out-$i.log"
    Write-Host "=== try $i / $MaxTries (concurrent=$Concurrent gcThreads=$GcThreads) ==="
    $p = Start-Process -FilePath $exe -WorkingDirectory $dir -RedirectStandardOutput $log -RedirectStandardError "$log.err" -PassThru -NoNewWindow
    if ($p.WaitForExit($timeoutSec * 1000)) {
        $code = $p.ExitCode
        if ($code -eq 0) { $ok++; Write-Host "  try $i OK (exit 0)"; if ($VerifyTrace -eq 0) { Remove-Item $log,"$log.err" -ErrorAction SilentlyContinue } }
        else { $av++; Write-Host "  try $i CRASH exit=$code -> kept $log" }
    } else {
        $hang++
        $procId = $p.Id
        Write-Host "  try $i HANG (pid $procId) -> kept $log"
        Stop-Process -Id $procId -Force -ErrorAction SilentlyContinue
    }
}
Write-Host "SUMMARY: ok=$ok av=$av hang=$hang of $MaxTries"
