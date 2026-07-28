<#
.SYNOPSIS
  Capture LXRGC's own EventPipe events out-of-process with dotnet-trace and
  analyze them offline (pause distribution + per-phase timings), the same way a
  production operator would profile the built-in .NET GC.

.DESCRIPTION
  LXR is a standalone GC. The runtime fires only ~1/3 of LXR's SuspendEE pauses
  as the implicit GCSuspendEEBegin/RestartEEEnd events (it coalesces a standalone
  GC's repeated concurrent suspensions), so an out-of-process trace of the
  built-in suspend events undercounts LXR pauses ~3x and gives zero insight into
  LXR's own phases. To fix this WITHOUT any runtime change, LXR emits its OWN GC
  dynamic events (surfaced as GCDynamicEvent, Name = "LXRGCPause"/"LXRGCPhase")
  through the standalone-GC event sink (IGCToCLREventSink::FireDynamicEvent). This
  script launches a sample under the full LXR config, attaches `dotnet-trace` by
  PID (attach-by-PID avoids the EventPipe attach-during-startup hang), then runs
  the TraceAnalyzer tool to reconstruct the pause distribution and per-phase
  breakdown entirely offline.

  Because the app is launched by PID (not by `dotnet-trace -- <app>`, whose child
  launcher does not propagate our CWD/GCName), attach happens ~1s in; the very
  first startup-burst pauses are therefore not in the trace. Those are still
  recorded completely by the in-process LXR_PAUSE_LOG, which this script captures
  in parallel for cross-checking.

.EXAMPLE
  ./capture-lxr-trace.ps1 -Scenario console -DurationSeconds 20
  ./capture-lxr-trace.ps1 -Scenario webapi  -DurationSeconds 30 -OutDir .\results\trace
#>
param(
    [ValidateSet("console", "webapi")]
    [string]$Scenario = "console",
    [int]$DurationSeconds = 20,
    [string]$OutDir = ".\results\trace",
    [int]$AttachDelayMs = 1000,
    [int]$GcThreads = 8
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

$dotnetTrace = Get-Command dotnet-trace -ErrorAction SilentlyContinue
if (-not $dotnetTrace) {
    $candidate = Join-Path $env:USERPROFILE ".dotnet\tools\dotnet-trace.exe"
    if (Test-Path $candidate) { $dotnetTrace = $candidate }
    else { throw "dotnet-trace not found. Install with: dotnet tool install -g dotnet-trace" }
}

# Ensure the offline analyzer is built.
$analyzerProj = Join-Path $root "tools\TraceAnalyzer\TraceAnalyzer.csproj"
$analyzerExe = Join-Path $root "tools\TraceAnalyzer\bin\Release\net8.0\TraceAnalyzer.exe"
if (-not (Test-Path $analyzerExe)) {
    Write-Host "Building TraceAnalyzer..."
    dotnet build $analyzerProj -c Release -v quiet | Out-Null
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDirFull = Convert-Path $OutDir

# Full paper-parity LXR configuration.
$lxrEnv = @{
    DOTNET_GCName        = "LXRGC.dll"
    LXR_CONCURRENT       = "1"
    LXR_EVAC             = "1"
    LXR_REMSET           = "1"
    LXR_LINE_REUSE       = "1"
    LXR_CONC_DECREMENTS  = "1"
    LXR_YOUNG_RC         = "1"
    LXR_NURSERY          = "1"
    LXR_NURSERY_COPY     = "1"
    LXR_MULTIEPOCH       = "1"
    LXR_GC_THREADS       = "$GcThreads"
    DOTNET_ReadyToRun    = "0"
}

switch ($Scenario) {
    "console" {
        $publishDir = Join-Path $root "samples\ConsoleApp\publish"
        $exe = Join-Path $publishDir "ConsoleApp.exe"
        $arguments = "$DurationSeconds"
    }
    "webapi" {
        $publishDir = Join-Path $root "samples\WebApi\publish"
        $exe = Join-Path $publishDir "WebApi.exe"
        $arguments = ""
        $lxrEnv["LXRGC_BENCH_DURATION_SECONDS"] = "$DurationSeconds"
        $lxrEnv["LXRGC_BENCH_LABEL"] = "trace"
    }
}
if (-not (Test-Path $exe)) { throw "Sample not published: $exe (run build-samples.ps1 first)" }

$pauseLog = Join-Path $OutDirFull "$Scenario.pauselog"
$traceOut = Join-Path $OutDirFull "$Scenario.nettrace"
$statsOut = Join-Path $OutDirFull "$Scenario-phasestats.json"
foreach ($f in @($pauseLog, $traceOut, $statsOut)) { if (Test-Path $f) { Remove-Item $f -Force } }
$lxrEnv["LXR_PAUSE_LOG"] = $pauseLog

Write-Host "Launching $Scenario ($DurationSeconds s) under full LXR config..."
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exe
$psi.WorkingDirectory = $publishDir
$psi.Arguments = $arguments
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
foreach ($k in $lxrEnv.Keys) { $psi.EnvironmentVariables[$k] = $lxrEnv[$k] }
$proc = [System.Diagnostics.Process]::Start($psi)
$stdoutTask = $proc.StandardOutput.ReadToEndAsync()
$stderrTask = $proc.StandardError.ReadToEndAsync()

Start-Sleep -Milliseconds $AttachDelayMs
Write-Host "Attaching dotnet-trace to PID $($proc.Id) (GC keyword 0x1)..."
# Trace slightly longer than the app to be safe; dotnet-trace stops when the
# target exits.
$traceDuration = [TimeSpan]::FromSeconds($DurationSeconds + 5).ToString("hh\:mm\:ss")
$dtArgs = @("collect", "--process-id", $proc.Id,
    "--providers", "Microsoft-Windows-DotNETRuntime:0x1:4",
    "--duration", $traceDuration, "--output", $traceOut)
& $dotnetTrace @dtArgs *> (Join-Path $OutDirFull "dotnet-trace.log") 2>&1

if (-not $proc.HasExited) { $proc.WaitForExit(60000) | Out-Null }
$stdout = $stdoutTask.Result
$stderr = $stderrTask.Result

$resultLine = ($stdout -split "`n" | Where-Object { $_ -match "^##RESULT##" } | Select-Object -First 1)
if ($resultLine) { Write-Host $resultLine.Trim() }
else { Write-Host "WARN: no ##RESULT## captured"; Write-Host $stderr }

if (-not (Test-Path $traceOut)) { throw "No trace produced at $traceOut" }

Write-Host ""
Write-Host "=== Offline analysis (out-of-process) ==="
& $analyzerExe $traceOut --json $statsOut

# Cross-check against the complete in-process pause log.
if (Test-Path $pauseLog) {
    $all = Get-Content $pauseLog
    Write-Host ""
    Write-Host "Cross-check - LXR_PAUSE_LOG (complete, incl. startup burst): $($all.Count) pauses"
    $all | Group-Object { ($_ -split ',')[0] } | ForEach-Object { Write-Host "  $($_.Name): $($_.Count)" }
}

Write-Host ""
Write-Host "Artifacts:"
Write-Host "  trace : $traceOut"
Write-Host "  stats : $statsOut"
