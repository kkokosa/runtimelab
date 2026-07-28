<#
.SYNOPSIS
  Render an LXRGC phase-insights HTML report from the phasestats JSON produced by
  capture-lxr-trace.ps1 (which runs TraceAnalyzer over an out-of-process
  dotnet-trace capture of LXR's own GCDynamicEvent pause/phase events).

  Self-contained (inline CSS/SVG, no external deps), matching the style of the
  main report. Gives the same kind of per-phase insight the built-in .NET GC
  exposes via its mark/sweep events - only here every datum came from LXR's own
  events, analyzed entirely outside the app.

.EXAMPLE
  ./generate-phase-report.ps1 -StatsJson .\results\trace\console-phasestats.json
#>
param(
    [string]$StatsJson = "$PSScriptRoot\results\trace\console-phasestats.json",
    [string]$OutHtml = "$PSScriptRoot\results\phase-report.html"
)

$ErrorActionPreference = "Stop"
if (-not (Test-Path $StatsJson)) { throw "Stats JSON not found: $StatsJson (run capture-lxr-trace.ps1 first)" }
$stats = Get-Content $StatsJson -Raw | ConvertFrom-Json

function HtmlEnc([string]$s) { if ($null -eq $s) { return "" } [System.Web.HttpUtility]::HtmlEncode($s) }
Add-Type -AssemblyName System.Web

# ---- Phase aggregates -> sorted list ----
$phaseRows = @()
if ($stats.phases) {
    foreach ($p in $stats.phases.PSObject.Properties) {
        $phaseRows += [pscustomobject]@{
            Name       = $p.Name
            Count      = [int]$p.Value.count
            TotalMs    = [double]$p.Value.totalMs
            MaxMs      = [double]$p.Value.maxMs
            Concurrent = ([int]$p.Value.concurrent -eq 1)
        }
    }
}
$phaseRows = $phaseRows | Sort-Object -Property TotalMs -Descending

$maxTotal = ($phaseRows | Measure-Object -Property TotalMs -Maximum).Maximum
if (-not $maxTotal -or $maxTotal -le 0) { $maxTotal = 1 }

# ---- Horizontal bar chart (inline SVG) of total ms per phase ----
$barH = 26; $gap = 8; $labelW = 190; $chartW = 520; $leftPad = 8
$svgH = ($phaseRows.Count * ($barH + $gap)) + $gap
$svg = New-Object System.Text.StringBuilder
[void]$svg.Append("<svg width='$([int]($labelW + $chartW + 90))' height='$svgH' xmlns='http://www.w3.org/2000/svg' font-family='Segoe UI, sans-serif' font-size='13'>")
$y = $gap
foreach ($r in $phaseRows) {
    $w = [int](($r.TotalMs / $maxTotal) * $chartW)
    if ($w -lt 1 -and $r.TotalMs -gt 0) { $w = 1 }
    $color = if ($r.Concurrent) { "#27ae60" } else { "#e94f2b" }
    $barX = $labelW + $leftPad
    [void]$svg.Append("<text x='$($labelW)' y='$($y + $barH/2 + 4)' text-anchor='end' fill='#333'>$(HtmlEnc $r.Name)</text>")
    [void]$svg.Append("<rect x='$barX' y='$y' width='$w' height='$barH' fill='$color' rx='3'/>")
    [void]$svg.Append("<text x='$($barX + $w + 6)' y='$($y + $barH/2 + 4)' fill='#333'>$([math]::Round($r.TotalMs,2)) ms (max $([math]::Round($r.MaxMs,2)))</text>")
    $y += $barH + $gap
}
[void]$svg.Append("</svg>")

# ---- Pause-by-type table ----
$pauseRowsHtml = ""
if ($stats.lxrPauseByType) {
    foreach ($p in $stats.lxrPauseByType.PSObject.Properties) {
        $pauseRowsHtml += "<tr><td>$(HtmlEnc $p.Name)</td><td>$($p.Value.count)</td><td>$([math]::Round([double]$p.Value.maxMs,2))</td><td>$([math]::Round([double]$p.Value.totalMs,2))</td></tr>`n"
    }
}

# ---- Pause distribution summary ----
$lxrSamples = @($stats.lxrPauseSamplesMs)
$suspendSamples = @($stats.suspendPauseSamplesMs)
function Pctl([double[]]$data, [double]$p) {
    if (-not $data -or $data.Count -eq 0) { return 0 }
    $s = $data | Sort-Object
    $rank = ($p / 100.0) * ($s.Count - 1)
    $lo = [math]::Floor($rank); $hi = [math]::Ceiling($rank)
    if ($lo -eq $hi) { return $s[$lo] }
    return $s[$lo] + ($rank - $lo) * ($s[$hi] - $s[$lo])
}
$lxrMax = if ($lxrSamples.Count) { ($lxrSamples | Measure-Object -Maximum).Maximum } else { 0 }
$lxrP99 = Pctl $lxrSamples 99
$lxrP50 = Pctl $lxrSamples 50
$suspMax = if ($suspendSamples.Count) { ($suspendSamples | Measure-Object -Maximum).Maximum } else { 0 }

$html = @"
<!DOCTYPE html>
<html lang='en'><head><meta charset='utf-8'>
<title>LXRGC Phase Insights (out-of-process EventPipe)</title>
<style>
  body { font-family: 'Segoe UI', sans-serif; margin: 2rem; color: #222; background: #fafafa; }
  h1 { font-size: 1.5rem; } h2 { font-size: 1.15rem; margin-top: 1.8rem; }
  .note { background: #eef4ff; border-left: 4px solid #2b7de9; padding: .7rem 1rem; border-radius: 4px; font-size: .92rem; max-width: 900px; }
  table { border-collapse: collapse; margin: .6rem 0; }
  th, td { border: 1px solid #ddd; padding: .35rem .7rem; text-align: right; }
  th:first-child, td:first-child { text-align: left; }
  th { background: #f0f0f0; }
  .legend span { display: inline-block; width: 12px; height: 12px; border-radius: 2px; margin: 0 4px -1px 12px; }
  .card { background: #fff; border: 1px solid #e2e2e2; border-radius: 6px; padding: 1rem 1.2rem; margin: .6rem 0; max-width: 900px; }
  code { background: #f2f2f2; padding: 0 .25rem; border-radius: 3px; }
</style></head><body>
<h1>LXRGC Phase Insights &mdash; measured out-of-process</h1>
<div class='note'>
Every datum below came from <b>LXR's own EventPipe events</b>
(<code>GCDynamicEvent</code> named <code>LXRGCPause</code> / <code>LXRGCPhase</code>),
emitted through the standard standalone-GC event sink
(<code>IGCToCLREventSink::FireDynamicEvent</code>) with <b>no runtime change</b>, then
captured with <code>dotnet-trace</code> and analyzed entirely offline &mdash; exactly
how one would profile the built-in .NET GC. This closes the ~3&times; undercount you get
from the runtime's implicit <code>GCSuspendEEBegin/RestartEEEnd</code> events (it coalesces
a standalone GC's repeated concurrent suspensions).
</div>

<h2>Per-phase time (per captured window)</h2>
<div class='legend'>
  <span style='background:#e94f2b'></span>STW (in-pause)
  <span style='background:#27ae60'></span>off-pause (concurrent, mutators live)
</div>
<div class='card'>$($svg.ToString())</div>

<h2>Pause distribution</h2>
<div class='card'>
<table>
<tr><th>Source</th><th>count</th><th>max ms</th><th>p99 ms</th><th>p50 ms</th></tr>
<tr><td>LXR own pause events (all pause kinds)</td><td>$($lxrSamples.Count)</td><td>$([math]::Round($lxrMax,2))</td><td>$([math]::Round($lxrP99,2))</td><td>$([math]::Round($lxrP50,2))</td></tr>
<tr><td>Runtime GCSuspendEE&rarr;GCRestartEE (implicit)</td><td>$($suspendSamples.Count)</td><td>$([math]::Round($suspMax,2))</td><td>-</td><td>-</td></tr>
</table>
<p style='font-size:.88rem;color:#555;max-width:820px'>Both rows come from the same out-of-process trace. When they diverge, the implicit
suspend events are undercounting LXR's real pauses.</p>
</div>

<h2>Pauses by LXR type</h2>
<div class='card'>
<table>
<tr><th>Pause type</th><th>count</th><th>max ms</th><th>total ms</th></tr>
$pauseRowsHtml
</table>
</div>

</body></html>
"@

New-Item -ItemType Directory -Force -Path (Split-Path $OutHtml) | Out-Null
Set-Content -Path $OutHtml -Value $html -Encoding UTF8
Write-Host "Wrote $OutHtml"
