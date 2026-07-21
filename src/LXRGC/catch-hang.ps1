param([int]$MaxTries = 8, [int]$Duration = 120, [int]$GcThreads = 16)
Get-ChildItem Env: | Where-Object Name -like "LXR_*" | Remove-Item -ErrorAction SilentlyContinue
$exe = "C:\github\runtimelab\src\LXRGC\samples\WebApi\publish\WebApi.exe"
$dir = Split-Path $exe
$dump = Join-Path $env:USERPROFILE ".dotnet\tools\dotnet-dump.exe"
$env:DOTNET_GCName = "LXRGC.dll"
$env:LXR_GC_THREADS = "$GcThreads"
$env:LXR_CONCURRENT = "1"
$env:LXR_EVAC = "1"
$env:LXR_REMSET = "1"
$env:LXR_FAULT_DIAG = "1"
$env:LXRGC_BENCH_DURATION_SECONDS = "$Duration"
$timeoutSec = $Duration + 60
for ($i = 1; $i -le $MaxTries; $i++) {
    $env:LXRGC_BENCH_LABEL = "hang$i"
    $log = "C:\github\runtimelab\src\LXRGC\hang-out-$i.log"
    Write-Host "=== try $i / $MaxTries (watchdog=$timeoutSec s, gcThreads=$GcThreads) ==="
    $p = Start-Process -FilePath $exe -WorkingDirectory $dir -RedirectStandardOutput $log -RedirectStandardError "$log.err" -PassThru -NoNewWindow
    if ($p.WaitForExit($timeoutSec * 1000)) {
        Write-Host "  try $i exit=$($p.ExitCode)"
    } else {
        $procId = $p.Id
        # Sample CPU twice, 3s apart, to distinguish spin (livelock) from wait (deadlock).
        $t1 = (Get-Process -Id $procId).TotalProcessorTime.TotalSeconds
        Start-Sleep -Seconds 3
        $t2 = (Get-Process -Id $procId).TotalProcessorTime.TotalSeconds
        $cpuDelta = [math]::Round($t2 - $t1, 2)
        Write-Host "  try $i HANG -> cpu-delta over 3s = ${cpuDelta}s ($(if($cpuDelta -gt 1.5){'SPINNING'}else{'IDLE/deadlock'}))"
        $cdb = "C:\Program Files\WindowsApps\Microsoft.WinDbg_1.2606.22001.0_x64__8wekyb3d8bbwe\amd64\cdb.exe"
        $out = "C:\github\runtimelab\src\LXRGC\hang-$procId.stacks.txt"
        & $cdb -p $procId -c ".symfix; .reload; da /c 100 poi(LXRGC!g_lxrPhase); ~*k 40; qd" 2>&1 |
            Out-File -Encoding utf8 $out
        Write-Host "  stacks saved: $out"
        Stop-Process -Id $procId -Force -ErrorAction SilentlyContinue
        return $out
    }
}
Write-Host "no hang caught in $MaxTries tries"
