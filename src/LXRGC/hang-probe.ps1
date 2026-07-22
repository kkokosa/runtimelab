param(
    [int]$MaxTries = 20,
    [int]$Duration = 45,
    [int]$GcThreads = 16
)
Get-ChildItem Env: | Where-Object { ($_.Name -like 'LXR*' -and $_.Name -ne 'LXR_DUMP_ON_GAP') -or $_.Name -eq 'DOTNET_GCName' } | ForEach-Object { Remove-Item "Env:$($_.Name)" -ErrorAction SilentlyContinue }
$exe = "C:\github\runtimelab\src\LXRGC\samples\WebApi\publish\WebApi.exe"
$dir = Split-Path $exe
$env:DOTNET_GCName = "LXRGC.dll"
$env:LXR_GC_THREADS = "$GcThreads"
$env:LXR_CONCURRENT = "1"
$env:LXR_EVAC = "1"
$env:LXR_REMSET = "1"
$env:LXR_LINE_REUSE = "1"
$env:LXR_CONC_DECREMENTS = "1"
$env:LXR_YOUNG_RC = "1"
$env:LXR_WATCHDOG = "1"
$env:DOTNET_ReadyToRun = "0"
$env:LXRGC_BENCH_DURATION_SECONDS = "$Duration"
$timeoutSec = $Duration + 40
$hang = 0; $av = 0; $ok = 0
for ($i = 1; $i -le $MaxTries; $i++) {
    $log = "C:\github\runtimelab\src\LXRGC\hang-out-$i.log"
    Write-Host "=== try $i / $MaxTries (gcThreads=$GcThreads) ==="
    $p = Start-Process -FilePath $exe -WorkingDirectory $dir -RedirectStandardOutput $log -RedirectStandardError "$log.err" -PassThru -NoNewWindow
    if ($p.WaitForExit($timeoutSec * 1000)) {
        $code = $p.ExitCode
        if ($code -eq 0) { $ok++; Write-Host "  try $i OK (exit 0)"; Remove-Item $log,"$log.err" -ErrorAction SilentlyContinue }
        else { $av++; Write-Host "  try $i CRASH exit=$code -> kept $log" }
    } else {
        $hang++
        $procId = $p.Id
        # give the watchdog time to print the stuck phase (20s stall threshold)
        Write-Host "  try $i not exited; waiting 25s for watchdog..."
        Start-Sleep -Seconds 25
        $stuck = (Select-String -Path "$log.err" -Pattern 'WATCHDOG' -EA SilentlyContinue | Select-Object -Last 2).Line
        Write-Host "  try $i HANG (pid $procId) stuck=[$stuck] -> collecting dump"
        $dump = "C:\github\runtimelab\src\LXRGC\hang-$i.dmp"
        # dotnet-dump is a managed tool; scrub LXR/GCName env so it can start the default runtime
        $saved = @{}
        Get-ChildItem Env: | Where-Object { $_.Name -like 'LXR*' -or $_.Name -eq 'DOTNET_GCName' -or $_.Name -eq 'DOTNET_ReadyToRun' } | ForEach-Object { $saved[$_.Name] = $_.Value; Remove-Item "Env:$($_.Name)" -EA SilentlyContinue }
        & "$env:USERPROFILE\.dotnet\tools\dotnet-dump.exe" collect -p $procId -o $dump --type Full 2>&1 | Select-Object -Last 5
        foreach ($k in $saved.Keys) { Set-Item "Env:$k" $saved[$k] }
        Stop-Process -Id $procId -Force -ErrorAction SilentlyContinue
        break
    }
}
Write-Host "SUMMARY: ok=$ok av=$av hang=$hang of $MaxTries"
