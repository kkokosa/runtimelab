param([int]$Iterations = 8, [int]$Duration = 120)
Get-ChildItem Env: | Where-Object Name -like "LXR_*" | Remove-Item -ErrorAction SilentlyContinue
Remove-Item Env:DOTNET_GCName -ErrorAction SilentlyContinue
Remove-Item Env:LXRGC_BENCH_DURATION_SECONDS -ErrorAction SilentlyContinue
Remove-Item Env:LXRGC_BENCH_LABEL -ErrorAction SilentlyContinue

$exe = "C:\github\runtimelab\src\LXRGC\samples\WebApi\publish\WebApi.exe"
$dir = Split-Path $exe
$env:DOTNET_GCName = "LXRGC.dll"
$env:LXR_GC_THREADS = "16"
$env:LXR_CONCURRENT = "1"
$env:LXR_EVAC = "1"
$env:LXR_REMSET = "1"
$env:LXR_FAULT_DIAG = "1"
$env:LXRGC_BENCH_DURATION_SECONDS = "$Duration"

$pass = 0; $fail = 0; $hang = 0
$timeoutSec = $Duration + 90
for ($i = 1; $i -le $Iterations; $i++) {
    $env:LXRGC_BENCH_LABEL = "loop$i"
    Write-Host "=== iteration $i / $Iterations (dur=$Duration s, watchdog=$timeoutSec s) ==="
    $log = "C:\github\runtimelab\src\LXRGC\loop-out-$i.log"
    $p = Start-Process -FilePath $exe -WorkingDirectory $dir -RedirectStandardOutput $log -RedirectStandardError "$log.err" -PassThru -NoNewWindow
    if ($p.WaitForExit($timeoutSec * 1000)) {
        $code = $p.ExitCode
        if ($code -eq 0) { $pass++; Write-Host "  iter $i PASS (exit 0)" }
        else { $fail++; Write-Host "  iter $i FAIL exit=$code (0x$('{0:X}' -f $code))" }
    } else {
        $hang++
        $procId = $p.Id
        Write-Host "  iter $i HANG (exceeded $timeoutSec s) killing Id=$procId"
        Stop-Process -Id $procId -Force -ErrorAction SilentlyContinue
        $p.WaitForExit(10000) | Out-Null
    }
}
Write-Host "=== DONE pass=$pass fail=$fail hang=$hang ==="
