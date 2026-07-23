param([int]$Iterations = 8, [int]$Duration = 55, [string]$ParRC = "1")
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
$env:LXR_LINE_REUSE = "1"
$env:LXR_CONC_DECREMENTS = "1"
$env:LXR_YOUNG_RC = "1"
$env:LXR_NURSERY = "1"
$env:LXR_MULTIEPOCH = "1"
$env:LXR_VERIFY_TRACE = "1"
$env:LXR_PARALLEL_RC = "$ParRC"
$env:DOTNET_ReadyToRun = "0"
$env:LXRGC_BENCH_DURATION_SECONDS = "$Duration"

$pass = 0; $fail = 0; $hang = 0
$timeoutSec = $Duration + 90
for ($i = 1; $i -le $Iterations; $i++) {
    $env:LXRGC_BENCH_LABEL = "valg$i"
    $log = "C:\github\runtimelab\src\LXRGC\valg-out-$i.log"
    $p = Start-Process -FilePath $exe -WorkingDirectory $dir -RedirectStandardOutput $log -RedirectStandardError "$log.err" -PassThru -NoNewWindow
    if ($p.WaitForExit($timeoutSec * 1000)) {
        $code = $p.ExitCode
        $av = Select-String -Path "$log.err" -Pattern 'ACCESS_VIOLATION|0xC0000005' -Quiet
        $verify = (Select-String -Path "$log.err" -Pattern 'offenders=([1-9])|misses=([1-9])' -Quiet)
        if ($code -eq 0 -and -not $av -and -not $verify) { $pass++; Write-Host "  iter $i PASS" }
        else { $fail++; Write-Host "  iter $i FAIL exit=$code av=$av verify=$verify" }
    } else {
        $hang++
        $procId = $p.Id
        Write-Host "  iter $i HANG killing Id=$procId"
        Stop-Process -Id $procId -Force -ErrorAction SilentlyContinue
        $p.WaitForExit(10000) | Out-Null
    }
}
Write-Host "=== DONE ParRC=$ParRC pass=$pass fail=$fail hang=$hang ==="
