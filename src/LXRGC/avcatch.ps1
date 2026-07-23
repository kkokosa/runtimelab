param([int]$MaxTries = 120, [int]$Duration = 45)
Get-ChildItem Env: | Where-Object { $_.Name -like 'LXR*' -or $_.Name -eq 'DOTNET_GCName' } | ForEach-Object { Remove-Item "Env:$($_.Name)" -ErrorAction SilentlyContinue }
$exe = "C:\github\runtimelab\src\LXRGC\samples\WebApi\publish\WebApi.exe"
$dir = Split-Path $exe
$env:DOTNET_GCName = "LXRGC.dll"
$env:LXR_GC_THREADS = "16"
$env:LXR_CONCURRENT = "1"; $env:LXR_EVAC = "1"; $env:LXR_REMSET = "1"
$env:LXR_LINE_REUSE = "1"; $env:LXR_CONC_DECREMENTS = "1"; $env:LXR_YOUNG_RC = "1"
$env:LXR_AV_STACKS = "1"
$env:DOTNET_ReadyToRun = "0"
$env:LXRGC_BENCH_DURATION_SECONDS = "$Duration"
$timeoutSec = $Duration + 40
for ($i = 1; $i -le $MaxTries; $i++) {
    $err = "C:\github\runtimelab\src\LXRGC\avc-$i.err"
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe; $psi.WorkingDirectory = $dir; $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
    $p = New-Object System.Diagnostics.Process; $p.StartInfo = $psi
    $sbO = New-Object System.Text.StringBuilder; $sbE = New-Object System.Text.StringBuilder
    $oE = Register-ObjectEvent -InputObject $p -EventName OutputDataReceived -Action { if ($EventArgs.Data) { [void]$Event.MessageData.AppendLine($EventArgs.Data) } } -MessageData $sbO
    $eE = Register-ObjectEvent -InputObject $p -EventName ErrorDataReceived -Action { if ($EventArgs.Data) { [void]$Event.MessageData.AppendLine($EventArgs.Data) } } -MessageData $sbE
    [void]$p.Start(); $p.BeginOutputReadLine(); $p.BeginErrorReadLine()
    $exited = $p.WaitForExit($timeoutSec * 1000)
    if ($exited) { Start-Sleep -Milliseconds 400; $code = $p.ExitCode } else { try { $p.Kill() } catch {}; $code = "HANG" }
    Unregister-Event -SourceIdentifier $oE.Name; Unregister-Event -SourceIdentifier $eE.Name
    $out = $sbO.ToString(); $errtxt = $sbE.ToString()
    $hasResult = $out -match '##RESULT##'
    $hasAvDump = $errtxt -match '\[AV\]'
    if ($hasAvDump -or ($exited -and $code -ne 0 -and -not $hasResult)) {
        Set-Content -Path $err -Value ($out + "`n===STDERR===`n" + $errtxt)
        Write-Host "  try $i CRASH code=$code avDump=$hasAvDump -> $err"
        if ($hasAvDump) { Write-Host "=== AV STACK CAPTURED at try $i ==="; break }
    } else {
        Write-Host "  try $i OK (code=$code)"
    }
}
Write-Host "done"
