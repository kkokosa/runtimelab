param(
    [int]$MaxTries = 12,
    [int]$Duration = 120,
    [int]$GcThreads = 16,
    [int]$Concurrent = 0,
    [int]$Evac = 1
)
Get-ChildItem Env: | Where-Object { $_.Name -like 'LXR*' -or $_.Name -eq 'DOTNET_GCName' } | ForEach-Object { Remove-Item "Env:$($_.Name)" -ErrorAction SilentlyContinue }
$exe = "C:\github\runtimelab\src\LXRGC\samples\WebApi\publish\WebApi.exe"
$dir = Split-Path $exe
$env:DOTNET_GCName = "LXRGC.dll"
$env:LXR_GC_THREADS = "$GcThreads"
if ($Concurrent -ne 0) { $env:LXR_CONCURRENT = "1" }
if ($Evac -ne 0) { $env:LXR_EVAC = "1" }
$env:LXR_REMSET = "1"
$env:LXR_FAULT_DIAG = "1"
$env:LXRGC_BENCH_DURATION_SECONDS = "$Duration"
$timeoutSec = $Duration + 60
$hang = 0; $av = 0; $ok = 0
for ($i = 1; $i -le $MaxTries; $i++) {
    $log = "C:\github\runtimelab\src\LXRGC\stwfull-out-$i.log"
    Write-Host "=== try $i / $MaxTries (concurrent=$Concurrent gcThreads=$GcThreads) ==="
    $p = Start-Process -FilePath $exe -WorkingDirectory $dir -RedirectStandardOutput $log -RedirectStandardError "$log.err" -PassThru -NoNewWindow
    if ($p.WaitForExit($timeoutSec * 1000)) {
        $code = $p.ExitCode
        if ($code -eq 0) { $ok++; Write-Host "  try $i OK (exit 0)"; Remove-Item $log,"$log.err" -ErrorAction SilentlyContinue }
        else { $av++; Write-Host "  try $i CRASH exit=$code -> kept $log" }
    } else {
        $hang++
        $procId = $p.Id
        Write-Host "  try $i HANG (pid $procId) -> kept $log"
        Stop-Process -Id $procId -Force -ErrorAction SilentlyContinue
    }
}
Write-Host "SUMMARY: ok=$ok av=$av hang=$hang of $MaxTries"
