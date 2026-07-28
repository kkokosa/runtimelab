<#
.SYNOPSIS
  Render a combined, cross-GC phase-insights HTML report from the per-mode
  phasestats JSONs produced by capture-lxr-trace.ps1 (workstation, server, lxrgc).

.DESCRIPTION
  Every datum comes from the SAME out-of-process measurement pipeline: a
  dotnet-trace capture (GC keyword 0x1) analyzed offline by TraceAnalyzer.
    * The pause DISTRIBUTION for all three GCs is reconstructed uniformly from
      the runtime's GCSuspendEEStart->GCRestartEEStop events present in every
      trace (suspendPauseSamplesMs) - a true apples-to-apples cross-GC pause
      comparison, measured entirely outside the app.
    * LXR additionally emits its OWN pause + per-phase events
      (GCDynamicEvent LXRGCPause/LXRGCPhase) via IGCToCLREventSink::FireDynamicEvent
      with NO runtime change, giving mark/sweep-style phase insight the standalone
      GC would otherwise not expose - and closing the ~3x pause undercount you get
      from the runtime's coalesced implicit suspend events.

.EXAMPLE
  ./generate-phase-compare-report.ps1 `
     -WorkstationJson .\results\trace\workstation\webapi-phasestats.json `
     -ServerJson      .\results\trace\server\webapi-phasestats.json `
     -LxrJson         .\results\trace\lxr\webapi-phasestats.json `
     -OutHtml         .\results\phase-report.html
#>
param(
    [string]$WorkstationJson = "$PSScriptRoot\results\trace\workstation\webapi-phasestats.json",
    [string]$ServerJson      = "$PSScriptRoot\results\trace\server\webapi-phasestats.json",
    [string]$LxrJson         = "$PSScriptRoot\results\trace\lxr\webapi-phasestats.json",
    [string]$Scenario        = "WebApi",
    [string]$OutHtml         = "$PSScriptRoot\results\phase-report.html"
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Web
function HtmlEnc([string]$s) { if ($null -eq $s) { return "" } [System.Web.HttpUtility]::HtmlEncode($s) }

function Pctl([double[]]$data, [double]$p) {
    if (-not $data -or $data.Count -eq 0) { return 0 }
    $s = @($data | Sort-Object)
    $rank = ($p / 100.0) * ($s.Count - 1)
    $lo = [math]::Floor($rank); $hi = [math]::Ceiling($rank)
    if ($lo -eq $hi) { return $s[$lo] }
    return $s[$lo] + ($rank - $lo) * ($s[$hi] - $s[$lo])
}

function Load-Mode([string]$label, [string]$json) {
    if (-not (Test-Path $json)) { Write-Warning "Missing $label stats: $json"; return $null }
    $s = Get-Content $json -Raw | ConvertFrom-Json
    $susp = @($s.suspendPauseSamplesMs | ForEach-Object { [double]$_ })
    [pscustomobject]@{
        Label   = $label
        Stats   = $s
        Suspend = $susp
        Count   = $susp.Count
        Max     = if ($susp.Count) { ($susp | Measure-Object -Maximum).Maximum } else { 0 }
        Mean    = if ($susp.Count) { ($susp | Measure-Object -Average).Average } else { 0 }
        Total   = if ($susp.Count) { ($susp | Measure-Object -Sum).Sum } else { 0 }
        P50     = Pctl $susp 50
        P99     = Pctl $susp 99
    }
}

$modes = @()
$modes += Load-Mode "Workstation (built-in)" $WorkstationJson
$modes += Load-Mode "Server (built-in)" $ServerJson
$modes += Load-Mode "LXR (standalone)" $LxrJson
$modes = @($modes | Where-Object { $_ -ne $null })
if ($modes.Count -eq 0) { throw "No phasestats JSON found. Run capture-lxr-trace.ps1 for each GC mode first." }

$lxr = $modes | Where-Object { $_.Label -like "LXR*" } | Select-Object -First 1

# ---- Cross-GC pause comparison rows ----
$maxAcross = ($modes | Measure-Object -Property Max -Maximum).Maximum
if (-not $maxAcross -or $maxAcross -le 0) { $maxAcross = 1 }
$cmpRows = ""
foreach ($m in $modes) {
    $barW = [int](($m.Max / $maxAcross) * 240)
    if ($barW -lt 1 -and $m.Max -gt 0) { $barW = 1 }
    $color = if ($m.Label -like "LXR*") { "#e94f2b" } else { "#2b7de9" }
    $bar = "<div style='background:$color;height:14px;width:${barW}px;border-radius:3px;display:inline-block;vertical-align:middle'></div>"
    $cmpRows += "<tr><td>$(HtmlEnc $m.Label)</td><td>$($m.Count)</td><td>$([math]::Round($m.Max,2))</td><td>$([math]::Round($m.P99,2))</td><td>$([math]::Round($m.P50,2))</td><td>$([math]::Round($m.Mean,2))</td><td>$([math]::Round($m.Total,2))</td><td style='text-align:left'>$bar</td></tr>`n"
}

# ---- LXR phase breakdown (SVG) ----
$phaseSvg = "<p style='color:#777'>No LXR phase data.</p>"
$pauseRowsHtml = ""
if ($lxr) {
    $phaseRows = @()
    if ($lxr.Stats.phases) {
        foreach ($p in $lxr.Stats.phases.PSObject.Properties) {
            $phaseRows += [pscustomobject]@{
                Name = $p.Name; Count = [int]$p.Value.count
                TotalMs = [double]$p.Value.totalMs; MaxMs = [double]$p.Value.maxMs
                Concurrent = ([int]$p.Value.concurrent -eq 1)
            }
        }
    }
    $phaseRows = @($phaseRows | Sort-Object -Property TotalMs -Descending)
    if ($phaseRows.Count) {
        $maxTotal = ($phaseRows | Measure-Object -Property TotalMs -Maximum).Maximum
        if (-not $maxTotal -or $maxTotal -le 0) { $maxTotal = 1 }
        $barH = 26; $gap = 8; $labelW = 190; $chartW = 480; $leftPad = 8
        $svgH = ($phaseRows.Count * ($barH + $gap)) + $gap
        $sb = New-Object System.Text.StringBuilder
        [void]$sb.Append("<svg width='$([int]($labelW + $chartW + 130))' height='$svgH' xmlns='http://www.w3.org/2000/svg' font-family='Segoe UI, sans-serif' font-size='13'>")
        $y = $gap
        foreach ($r in $phaseRows) {
            $w = [int](($r.TotalMs / $maxTotal) * $chartW)
            if ($w -lt 1 -and $r.TotalMs -gt 0) { $w = 1 }
            $color = if ($r.Concurrent) { "#27ae60" } else { "#e94f2b" }
            $barX = $labelW + $leftPad
            [void]$sb.Append("<text x='$labelW' y='$($y + $barH/2 + 4)' text-anchor='end' fill='#333'>$(HtmlEnc $r.Name)</text>")
            [void]$sb.Append("<rect x='$barX' y='$y' width='$w' height='$barH' fill='$color' rx='3'/>")
            [void]$sb.Append("<text x='$($barX + $w + 6)' y='$($y + $barH/2 + 4)' fill='#333'>$([math]::Round($r.TotalMs,2)) ms (max $([math]::Round($r.MaxMs,2)))</text>")
            $y += $barH + $gap
        }
        [void]$sb.Append("</svg>")
        $phaseSvg = $sb.ToString()
    }
    if ($lxr.Stats.lxrPauseByType) {
        foreach ($p in $lxr.Stats.lxrPauseByType.PSObject.Properties) {
            $pauseRowsHtml += "<tr><td>$(HtmlEnc $p.Name)</td><td>$($p.Value.count)</td><td>$([math]::Round([double]$p.Value.maxMs,2))</td><td>$([math]::Round([double]$p.Value.totalMs,2))</td></tr>`n"
        }
    }
}

$genTime = (Get-Date).ToString("yyyy-MM-dd HH:mm")
$html = @"
<!DOCTYPE html>
<html lang='en'><head><meta charset='utf-8'>
<title>GC Pause & Phase Insights - $(HtmlEnc $Scenario) (out-of-process EventPipe)</title>
<style>
  body { font-family: 'Segoe UI', sans-serif; margin: 2rem; color: #222; background: #fafafa; }
  h1 { font-size: 1.5rem; } h2 { font-size: 1.15rem; margin-top: 1.8rem; }
  .note { background: #eef4ff; border-left: 4px solid #2b7de9; padding: .7rem 1rem; border-radius: 4px; font-size: .92rem; max-width: 960px; }
  table { border-collapse: collapse; margin: .6rem 0; }
  th, td { border: 1px solid #ddd; padding: .35rem .7rem; text-align: right; }
  th:first-child, td:first-child { text-align: left; }
  th { background: #f0f0f0; }
  .legend span { display: inline-block; width: 12px; height: 12px; border-radius: 2px; margin: 0 4px -1px 12px; }
  .card { background: #fff; border: 1px solid #e2e2e2; border-radius: 6px; padding: 1rem 1.2rem; margin: .6rem 0; max-width: 960px; }
  code { background: #f2f2f2; padding: 0 .25rem; border-radius: 3px; }
  .foot { color:#888; font-size:.82rem; margin-top:2rem; }
</style></head><body>
<h1>GC Pause &amp; Phase Insights &mdash; $(HtmlEnc $Scenario), measured out-of-process</h1>
<div class='note'>
All three GCs are profiled by the <b>same out-of-process pipeline</b>: a
<code>dotnet-trace</code> capture (GC keyword 0x1) analyzed offline. The pause
distribution is reconstructed uniformly from the runtime's
<code>GCSuspendEEStart&rarr;GCRestartEEStop</code> events in every trace, so the
comparison is apples-to-apples. LXR <i>additionally</i> emits its own
<code>GCDynamicEvent</code> pause/phase events
(<code>LXRGCPause</code>/<code>LXRGCPhase</code>) through
<code>IGCToCLREventSink::FireDynamicEvent</code> &mdash; <b>with no runtime change</b> &mdash;
giving mark/sweep-style phase insight and closing the ~3&times; undercount from the
runtime's coalesced implicit suspend events.
</div>

<h2>Cross-GC pause distribution (uniform, from suspend events)</h2>
<div class='card'>
<table>
<tr><th>GC mode</th><th>pauses</th><th>max ms</th><th>p99 ms</th><th>p50 ms</th><th>mean ms</th><th>total ms</th><th style='text-align:left'>max (bar)</th></tr>
$cmpRows
</table>
<p style='font-size:.88rem;color:#555;max-width:900px'>Every row is measured identically and entirely outside the app. This window is a
short steady-state slice; absolute counts scale with the capture length. LXR's
worst pause here is driven by its trace-finish/evacuate/sweep work (see phase
breakdown), which can spike above the built-in GCs on heap-growth cycles &mdash; an
honest, actionable signal, not a hidden metric.</p>
</div>

<h2>LXR per-phase time (where each LXR pause goes)</h2>
<div class='legend'>
  <span style='background:#e94f2b'></span>STW (in-pause)
  <span style='background:#27ae60'></span>off-pause (concurrent, mutators live)
</div>
<div class='card'>$phaseSvg</div>

<h2>LXR pauses by type</h2>
<div class='card'>
<table>
<tr><th>Pause type</th><th>count</th><th>max ms</th><th>total ms</th></tr>
$pauseRowsHtml
</table>
</div>

<div class='foot'>Generated $genTime &middot; sources: workstation/server/lxr phasestats JSON from TraceAnalyzer.</div>
</body></html>
"@

New-Item -ItemType Directory -Force -Path (Split-Path $OutHtml) | Out-Null
Set-Content -Path $OutHtml -Value $html -Encoding UTF8
Write-Host "Wrote $OutHtml"
foreach ($m in $modes) {
    Write-Host ("  {0,-24} pauses={1,-3} max={2,6}ms p50={3,6}ms total={4,7}ms" -f $m.Label, $m.Count, [math]::Round($m.Max,2), [math]::Round($m.P50,2), [math]::Round($m.Total,2))
}
