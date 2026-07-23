param(
    [int]$MaxTries = 60,
    [int]$Duration = 45,
    [int]$GcThreads = 16,
    [string]$Evac = "1",
    [string]$Tag = "va"
)
# Scrub any inherited LXR/GC env so we set a clean, explicit config.
Get-ChildItem Env: | Where-Object { $_.Name -like 'LXR*' -or $_.Name -eq 'DOTNET_GCName' } | ForEach-Object { Remove-Item "Env:$($_.Name)" -ErrorAction SilentlyContinue }
$exe = "C:\github\runtimelab\src\LXRGC\samples\WebApi\publish\WebApi.exe"
$dir = Split-Path $exe
$env:DOTNET_GCName = "LXRGC.dll"
$env:LXR_GC_THREADS = "$GcThreads"
$env:LXR_CONCURRENT = "1"
$env:LXR_EVAC = "$Evac"
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
    $log = "C:\github\runtimelab\src\LXRGC\$Tag-out-$i.log"
    $errlog = "$log.err"
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.WorkingDirectory = $dir
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = New-Object System.Diagnostics.Process
    $p.StartInfo = $psi
    $sbOut = New-Object System.Text.StringBuilder
    $sbErr = New-Object System.Text.StringBuilder
    $oEvt = Register-ObjectEvent -InputObject $p -EventName OutputDataReceived -Action { if ($EventArgs.Data) { [void]$Event.MessageData.AppendLine($EventArgs.Data) } } -MessageData $sbOut
    $eEvt = Register-ObjectEvent -InputObject $p -EventName ErrorDataReceived -Action { if ($EventArgs.Data) { [void]$Event.MessageData.AppendLine($EventArgs.Data) } } -MessageData $sbErr
    [void]$p.Start()
    $p.BeginOutputReadLine(); $p.BeginErrorReadLine()
    $exited = $p.WaitForExit($timeoutSec * 1000)
    if ($exited) {
        Start-Sleep -Milliseconds 300  # let async readers flush
        $code = $p.ExitCode
    }
    Unregister-Event -SourceIdentifier $oEvt.Name; Unregister-Event -SourceIdentifier $eEvt.Name
    $out = $sbOut.ToString(); $err = $sbErr.ToString()
    Set-Content -Path $log -Value $out
    Set-Content -Path $errlog -Value $err
    if (-not $exited) {
        $hang++
        Write-Host "  try $i HANG (pid $($p.Id)) -> kept $log"
        try { $p.Kill() } catch {}
        continue
    }
    $hasResult = $out -match '##RESULT##'
    $crashSig = ($err -match '0xC0000005') -or ($err -match 'Fatal error') -or ($err -match 'Access violation') -or ($code -ne 0 -and $code -ne $null)
    if ($hasResult -and -not $crashSig) {
        $ok++
        Write-Host "  try $i OK (code=$code)"
        Remove-Item $log,$errlog -ErrorAction SilentlyContinue
    } else {
        $av++
        Write-Host "  try $i CRASH (code=$code hasResult=$hasResult) -> kept $log"
    }
}
Write-Host "SUMMARY: ok=$ok av=$av hang=$hang of $MaxTries"
