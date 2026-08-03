<#
.SYNOPSIS
    Runs the LXRGC benchmark suite across THREE GC configurations
    (Workstation GC, Server GC, LXRGC) and five workloads: the two
    hand-rolled sample apps (ConsoleApp, WebApi) plus three GCPerfSim
    scenarios modeling more realistic allocation patterns (web-server
    request processing, a cache/large-object-heavy workload, and
    high-throughput transient object churn).

    For every (workload, GC mode) pair, the target process is launched with
    the right environment variables and a SINGLE out-of-process `dotnet-trace`
    session attaches to it by PID for the whole run, capturing one .nettrace
    that TraceAnalyzer reconstructs offline into every metric:
      * per-pause STW latency - built-in GCs from GCSuspendEEStart->
        GCRestartEEStop, LXRGC from its own GCDynamic "LXRGCPause" events;
      * heap size / alloc rate / collection counts from the CLR GC events;
      * working set / committed bytes / a custom "operations" throughput
        counter from EventCounters riding the same session.
    There is NO dotnet-counters client and NO custom pause log: pause metrics
    are 100% event-sourced from dotnet-trace, uniform across all three GCs.
    This works identically for LXRGC because it fires the standard GC events
    (plus its own GCDynamic pause events) through the GC-EE event sink.

    Writes results\results-full.json (summary + percentiles + downsampled
    time series per run) and then renders results\report.html.

.EXAMPLE
    .\run-benchmarks.ps1 -DurationSeconds 600
#>
param(
    [int]$DurationSeconds = 600,
    [string]$OutDir = "$PSScriptRoot\results",
    [string[]]$Scenarios = @("console", "webapi", "gcperfsim-webserver", "gcperfsim-cache", "gcperfsim-churn", "zeroalloc", "dotllm-serve", "dotllm-serve-1_5b", "growing-cache", "gcperfsim-mt-throughput", "gcperfsim-mt-throughput-moderate"),
    [string[]]$GcModes = @("workstation", "server", "lxrgc"),
    [int]$CounterRefreshIntervalSeconds = 1,
    [string]$DotLlmModelFile = "$env:USERPROFILE\.dotllm\models\QuantFactory\SmolLM-135M-GGUF\SmolLM-135M.Q4_K_M.gguf",
    [int]$DotLlmPort = 8099,
    [string]$DotLlmLargeModelFile = "$env:USERPROFILE\.dotllm\models\Qwen\Qwen2.5-1.5B-Instruct-GGUF\qwen2.5-1.5b-instruct-q4_k_m.gguf",
    [int]$DotLlmLargePort = 8100,
    # Assembled DOTNET_ROOT that lets the external dotLLM tool run on the
    # custom pluggable-write-barrier runtime (so LXRGC can actually load)
    # while still resolving its net10 ASP.NET dependency via roll-forward.
    # It contains: the custom-barrier Microsoft.NETCore.App (+ LXRGC.dll)
    # and a stock net11 Microsoft.AspNetCore.App, both exposed as 11.0.0.
    # When present, ALL THREE GC modes launch dotLLM via
    # "<root>\dotnet.exe DotLLM.Cli.dll ..." on this same runtime for a fair
    # comparison. When empty/missing, dotllm falls back to its installed
    # apphost (LXRGC mode then cannot load - workstation/server only).
    [string]$DotLlmDotnetRoot = "C:\temp\lxr-dotnetroot"
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$gcDll = Join-Path $root "native\obj\Release\LXRGC.dll"
if (-not (Test-Path $gcDll)) { throw "LXRGC.dll not built. Run native\build.ps1 first." }

# Event-based, OUT-OF-PROCESS measurement via a SINGLE dotnet-trace session is the
# authoritative (and only) path. dotnet-trace attaches by PID and captures, over
# one EventPipe stream:
#   * CLR GC events (built-in GCs fire GCSuspendEE*/GCHeapStats/GCAllocationTick/
#     GCStart natively; LXR fires the same events through the GC->EE event sink) -
#     source pauses, heap size, alloc rate, collection counts;
#   * System.Runtime EventCounters (working-set, gc-committed) - the memory metrics
#     that have no GC *event*;
#   * the app's LXRGC.Bench "operations" IncrementingEventCounter - throughput.
# TraceAnalyzer reconstructs every series offline. A single session deliberately
# replaces the earlier dotnet-trace + dotnet-counters pair: two concurrent
# EventPipe diagnostic clients intermittently DEADLOCK the target at attach, which
# (with dotnet-counters blocking on the hung app) froze the whole suite.
$dotnetTrace = Get-Command dotnet-trace -ErrorAction SilentlyContinue
if (-not $dotnetTrace) {
    $candidate = Join-Path $env:USERPROFILE ".dotnet\tools\dotnet-trace.exe"
    if (-not (Test-Path $candidate)) { throw "dotnet-trace not found. Install via: dotnet tool install --global dotnet-trace" }
    $dotnetTrace = $candidate
}
else { $dotnetTrace = $dotnetTrace.Source }

$traceAnalyzerProj = Join-Path $root "tools\TraceAnalyzer\TraceAnalyzer.csproj"
$traceAnalyzerExe = Join-Path $root "tools\TraceAnalyzer\bin\Release\net8.0\TraceAnalyzer.exe"
if (-not (Test-Path $traceAnalyzerExe)) {
    Write-Host "Building TraceAnalyzer (offline event analyzer)..."
    dotnet build $traceAnalyzerProj -c Release -v quiet | Out-Null
    if (-not (Test-Path $traceAnalyzerExe)) { throw "Failed to build TraceAnalyzer at $traceAnalyzerExe" }
}

# dotllm (https://github.com/kkokosa/dotLLM) is only required for the
# "dotllm-serve"/"dotllm-serve-1_5b" scenarios - real-world, (near-)zero-
# allocation LLM inference servers at two different model sizes/context
# lengths. Resolved lazily so the rest of the suite still runs on machines
# that don't have it installed.
$dotLlmExe = $null
if ($Scenarios -contains "dotllm-serve" -or $Scenarios -contains "dotllm-serve-1_5b") {
    $dotLlmCmd = Get-Command dotllm -ErrorAction SilentlyContinue
    if ($dotLlmCmd) { $dotLlmExe = $dotLlmCmd.Source }
    else {
        $candidate = Join-Path $env:USERPROFILE ".dotnet\tools\dotllm.exe"
        if (Test-Path $candidate) { $dotLlmExe = $candidate }
    }
    if (-not $dotLlmExe) {
        throw "dotllm not found but a dotllm-serve* scenario was requested. Install via: dotnet tool install -g DotLLM.Cli --prerelease"
    }
    if ($Scenarios -contains "dotllm-serve" -and -not (Test-Path $DotLlmModelFile)) {
        throw "dotLLM model file not found at '$DotLlmModelFile'. Pull it first: dotllm model pull QuantFactory/SmolLM-135M-GGUF --file SmolLM-135M.Q4_K_M.gguf"
    }
    if ($Scenarios -contains "dotllm-serve-1_5b" -and -not (Test-Path $DotLlmLargeModelFile)) {
        throw "dotLLM 1.5B model file not found at '$DotLlmLargeModelFile'. Pull it first: dotllm model pull Qwen/Qwen2.5-1.5B-Instruct-GGUF --file qwen2.5-1.5b-instruct-q4_k_m.gguf"
    }
    # For GC-plugin loading (DOTNET_GCName=LXRGC.dll), CoreCLR resolves the
    # plugin relative to the app's own directory - not the apphost.exe's
    # folder in ~/.dotnet/tools, but the actual managed assembly's directory
    # under the global tool's .store layout. Copy LXRGC.dll there so the
    # zerogc GC mode can load for this scenario too.
    $dotLlmAppDll = Get-ChildItem (Join-Path $env:USERPROFILE ".dotnet\tools\.store\dotllm.cli") -Recurse -Filter "DotLLM.Cli.dll" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($dotLlmAppDll) {
        Copy-Item $gcDll -Destination $dotLlmAppDll.DirectoryName -Force
    }

    # Full-fidelity launch path: if an assembled DOTNET_ROOT is available,
    # run dotLLM as "<root>\dotnet.exe DotLLM.Cli.dll ..." on the custom
    # pluggable-write-barrier runtime so LXRGC actually loads. This is the
    # ONLY way to run the external (net10, ASP.NET-dependent) dotLLM tool on
    # the ABI-bumped net11 barrier runtime. All three GC modes use it for a
    # fair, apples-to-apples comparison. Refresh LXRGC.dll in the assembled
    # NETCore.App so it matches the just-built native GC.
    $dotLlmUseAssembledRoot = $false
    if ($DotLlmDotnetRoot -and (Test-Path (Join-Path $DotLlmDotnetRoot "dotnet.exe")) -and $dotLlmAppDll) {
        $rootDotnet = Join-Path $DotLlmDotnetRoot "dotnet.exe"
        $rootNetCore = Join-Path $DotLlmDotnetRoot "shared\Microsoft.NETCore.App\11.0.0"
        if (Test-Path $rootNetCore) {
            Copy-Item $gcDll -Destination $rootNetCore -Force
            $dotLlmUseAssembledRoot = $true
            Write-Host "dotLLM will run on assembled custom runtime: $DotLlmDotnetRoot" -ForegroundColor Green
        }
    }
    if (-not $dotLlmUseAssembledRoot) {
        Write-Host "WARNING: assembled DOTNET_ROOT not found - dotLLM LXRGC mode cannot load the custom GC; only workstation/server are meaningful." -ForegroundColor Yellow
    }
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$rawDir = Join-Path $OutDir "raw"
New-Item -ItemType Directory -Force -Path $rawDir | Out-Null

# --- Workload definitions -----------------------------------------------
# GCPerfSim scenarios are bounded by total allocation (-tagb), not wall
# time: this is the key safety mechanism that lets us run for many minutes
# without risking machine memory, since LXRGC never reclaims anything.
# Each scenario's -c (compute-between-allocations) value dominates wall
# time far more than GC pause overhead does, so all three GC modes finish
# in roughly comparable wall time even though LXRGC has zero pause time.
# BaseTagbFor600s below was calibrated so the run takes ~600s under the
# regular Workstation GC; it's scaled linearly by -DurationSeconds.
function New-Scenario($Id, $DisplayName, $Kind, $CmdArgs = $null, $BaseTagbFor600s = 0) {
    [pscustomobject]@{ Id = $Id; DisplayName = $DisplayName; Kind = $Kind; Args = $CmdArgs; BaseTagbFor600s = $BaseTagbFor600s }
}

$allScenarios = @(
    (New-Scenario "console" "Console: mixed alloc-heavy CLI workload" "console"),
    (New-Scenario "webapi" "ASP.NET Core (Kestrel) minimal API" "webapi"),
    (New-Scenario "gcperfsim-webserver" "GCPerfSim: web-server request simulation" "gcperfsim" `
        "-tc 4 -tlgb 0.5 -sohsr 200-3000 -sohsi 15 -at simple -c 300000" 7.6),
    (New-Scenario "gcperfsim-cache" "GCPerfSim: cache / large-object-heavy workload" "gcperfsim" `
        "-tc 2 -tlgb 0.3 -lohar 100 -sohsr 500-4000 -sohsi 8 -lohsr 100000-300000 -lohsi 4 -at simple -c 300000" 5.3),
    (New-Scenario "gcperfsim-churn" "GCPerfSim: high-throughput transient object churn" "gcperfsim" `
        "-tc 8 -tlgb 0.1 -lohar 0 -sohsr 200-600 -sohsi 200 -at simple -c 300000" 3.4),
    # --- Zero-alloc scenarios: the point isn't to stress the GC, it's the
    # opposite - when an app allocates almost nothing, the choice of GC
    # (including never collecting at all) shouldn't matter. These two
    # scenarios demonstrate that Workstation/Server/LXRGC all perform
    # near-identically when there's nothing to collect.
    (New-Scenario "zeroalloc" "Console: near-zero-allocation numeric compute (matrix multiply)" "zeroalloc"),
    (New-Scenario "dotllm-serve" "dotLLM: zero-alloc-inference HTTP server (github.com/kkokosa/dotLLM)" "dotllm-serve"),
    # A second, heavier dotLLM scenario: a real ~1GB quantized 1.5B-parameter
    # model (Qwen2.5-1.5B-Instruct, Q4_K_M) fed a few-hundred-token prompt
    # per request instead of a handful of words. Bigger weights (more model
    # state resident during inference) and a much longer prefill exercise a
    # meaningfully different allocation/compute profile than the tiny
    # 135M/short-prompt scenario above, while still being a genuinely
    # near-zero-managed-alloc inference workload (same NativeMemory-backed
    # tensor path).
    (New-Scenario "dotllm-serve-1_5b" "dotLLM: 1.5B model (~1GB, Q4_K_M), few-hundred-token context, zero-alloc-inference HTTP server" "dotllm-serve-large"),
    # A cache/session-store-style service that keeps accumulating long-lived
    # entries over its lifetime while handling ordinary "requests" that
    # allocate short-lived garbage - a very common real-world pattern (an
    # ever-growing in-memory cache/session store). Every gen2 collection
    # observed here is 100% naturally triggered by the CLR's own
    # allocation-budget/promotion heuristics; this program never calls
    # GC.Collect(). As the cache grows, gen2's live-object count grows with
    # it, so pauses under Workstation/Server GC naturally get longer and
    # longer over the run (LXRGC, which never collects, stays at 0ms).
    (New-Scenario "growing-cache" "Console: growing in-memory cache with real request workload (naturally escalating gen2 GCs)" "growingcache" "4000 512"),
    # A deliberately GC-bound "raw concurrent allocation throughput" burst:
    # 16 threads (matched to this machine's 16 logical cores) allocating as
    # fast as possible with NO compute delay between allocations (`-c 0`),
    # unlike the other GCPerfSim scenarios above which all use `-c 300000`
    # and are therefore compute-bound (allocation is a small fraction of
    # wall time, which is why Workstation and Server GC show no measurable
    # throughput gap in those scenarios). With `-c 0`, allocation/collection
    # cost dominates wall time, exposing Server GC's per-core heaps/parallel
    # GC threads: it clears ~30GB roughly 3-4x faster than Workstation GC's
    # single heap. This scenario intentionally runs as a short, sub-30-second
    # burst (scaled by BaseTagbFor600s, same as the other GCPerfSim
    # scenarios) rather than a full ~600s run, so it stays memory-safe for
    # LXRGC (which retains 100% of everything ever allocated) while still
    # producing hundreds of gen0 and dozens of gen1/gen2 collections to
    # measure. With LXRGC's lock-free per-thread arena allocator (see
    # README), it is now the fastest of the three GC modes here - a
    # zero-synchronization, zero-collection allocator wins decisively once
    # allocation/collection cost dominates wall time.
    (New-Scenario "gcperfsim-mt-throughput" "GCPerfSim: 16-thread concurrent allocation throughput burst (GC-bound, no compute delay)" "gcperfsim" `
        "-tc 16 -tlgb 0.3 -sohsr 100-2000 -sohsi 50 -lohar 0 -at simple -c 0" 45.0)
    # A more moderate, less artificial variant of the burst above: the same
    # 16-thread allocation pattern, but with -c 1000 (a modest compute delay
    # between allocations) instead of -c 0. Empirically (measured directly
    # against this LXRGC build), the GC-mode gap narrows but does not
    # vanish as compute is added: -c 0 gives a ~6.6x spread between GC
    # modes, -c 1000 narrows that to ~1.7x (Workstation ~3.0GB/s, Server
    # ~4.7GB/s, LXRGC ~5.2GB/s), and by -c 5000 the spread is down to noise
    # level (~13%) as compute fully dominates wall time. This scenario picks
    # -c 1000 as a "still clearly GC-bound but not a zero-compute extreme"
    # middle ground, illustrating that the GC-mode differences seen in
    # gcperfsim-mt-throughput are real but require allocation-dominated
    # workloads to surface - most realistic request-handling code (see the
    # other scenarios) dilutes them into noise.
    (New-Scenario "gcperfsim-mt-throughput-moderate" "GCPerfSim: 16-thread concurrent allocation throughput burst (GC-bound, moderate compute delay)" "gcperfsim" `
        "-tc 16 -tlgb 0.3 -sohsr 100-2000 -sohsi 50 -lohar 0 -at simple -c 1000" 45.0)
)

$gcModeDefs = @(
    [pscustomobject]@{ Id = "workstation"; DisplayName = "Workstation GC"; Env = @{ DOTNET_gcServer = "0" }; RemoveEnv = @("DOTNET_GCName") },
    [pscustomobject]@{ Id = "server";      DisplayName = "Server GC";      Env = @{ DOTNET_gcServer = "1" }; RemoveEnv = @("DOTNET_GCName") },
    # LXRGC recommended/sound config (matches the storm-validated parkdiag_exp.ps1 baseline).
    # NOTE: LXR_NURSERY_COPY is intentionally NOT set - RC-pause young-copy was found
    # unsound (stale mature->young remset edges) and is default-OFF in code. Young defrag
    # rides STW Evacuate instead. LXR_MARKER_PARK=1 enables paper-faithful in-window young
    # RC (§3.2.2); LXR_TRACE_MAX_SPAN=3 bounds the concurrent trace window.
    # LXR_OFFPAUSE_PARALLEL=1 runs the off-pause concurrent marker on the parallel
    # worker pool (paper §3.5); the shared-stack drain now honors the same prompt-
    # finish bail as the serial path, so the window closes within a few RC epochs
    # (footprint stays bounded) while marking ~3x faster than the serial marker.
    # (LXR_GC_GROWTH_PCT is left at its code default of 50 = adaptive budget: pinning it
    # to 0/32MiB was a storm-footprint diagnostic that forces collect-every-32MB and
    # inflates churn pauses to ~250ms, defeating LXR's low-latency purpose.)
    # Diagnostic knobs (LXR_VERIFY_TRACE/LXR_CADENCE/LXR_AV_*) are omitted here - they add
    # overhead and are for A/B debugging, not perf measurement.
    [pscustomobject]@{ Id = "lxrgc";      DisplayName = "LXRGC (full)"; Env = @{ DOTNET_GCName = "LXRGC.dll"; LXR_CONCURRENT = "1"; LXR_EVAC = "1"; LXR_REMSET = "1"; LXR_LINE_REUSE = "1"; LXR_CONC_DECREMENTS = "1"; LXR_YOUNG_RC = "1"; LXR_NURSERY = "1"; LXR_MULTIEPOCH = "1"; LXR_MARKER_PARK = "1"; LXR_TRACE_MAX_SPAN = "3"; LXR_OFFPAUSE_PARALLEL = "1"; LXR_GC_THREADS = "16"; DOTNET_ReadyToRun = "0" }; RemoveEnv = @("DOTNET_gcServer") }
)

$scenarioMap = @{}
foreach ($s in $allScenarios) { $scenarioMap[$s.Id] = $s }
$gcModeMap = @{}
foreach ($g in $gcModeDefs) { $gcModeMap[$g.Id] = $g }

# --- Helpers --------------------------------------------------------------

function Get-Percentile([double[]]$Values, [double]$Percentile) {
    if (-not $Values -or $Values.Count -eq 0) { return 0 }
    $sorted = $Values | Sort-Object
    $idx = [Math]::Ceiling($Percentile / 100.0 * $sorted.Count) - 1
    if ($idx -lt 0) { $idx = 0 }
    if ($idx -ge $sorted.Count) { $idx = $sorted.Count - 1 }
    return [double]$sorted[$idx]
}

function Get-Stats([double[]]$Values) {
    if (-not $Values -or $Values.Count -eq 0) {
        return [ordered]@{ Avg = 0; P50 = 0; P90 = 0; P99 = 0; Max = 0; Min = 0; Count = 0 }
    }
    $measure = $Values | Measure-Object -Average -Maximum -Minimum
    return [ordered]@{
        Avg   = $measure.Average
        P50   = Get-Percentile $Values 50
        P90   = Get-Percentile $Values 90
        P99   = Get-Percentile $Values 99
        Max   = $measure.Maximum
        Min   = $measure.Minimum
        Count = $Values.Count
    }
}

# Downsample a (t,v) series to at most $MaxPoints points (evenly spaced) so
# the final HTML report stays a reasonable size even for 600s/1Hz series.
function Get-Downsampled($Series, [int]$MaxPoints = 150) {
    if ($Series.Count -le $MaxPoints) { return $Series }
    $step = [Math]::Ceiling($Series.Count / [double]$MaxPoints)
    $out = @()
    for ($i = 0; $i -lt $Series.Count; $i += $step) { $out += , $Series[$i] }
    return $out
}

# Attaches dotnet-trace to a running process BY PID (attach-by-PID avoids the
# EventPipe attach-during-startup hang) and captures the CLR GC EventPipe stream
# out-of-process to a .nettrace. --duration auto-stops + flushes a clean rundown
# after $DurationSeconds, so the trace is complete even though the target app is
# still alive. Returns the background dotnet-trace process (wait on it later).
function Start-TraceCollector([int]$TargetPid, [string]$TraceOut, [int]$DurationSeconds) {
    if (Test-Path $TraceOut) { Remove-Item $TraceOut -Force -ErrorAction SilentlyContinue }
    $dur = [TimeSpan]::FromSeconds([Math]::Max(1, $DurationSeconds)).ToString("hh\:mm\:ss")
    # One EventPipe session carrying everything:
    #   * Microsoft-Windows-DotNETRuntime:0x1:5  - GC keyword @ verbose: GCSuspendEE*/
    #     GCHeapStats/GCStart/GCEnd + GCAllocationTick + GCDynamicEvent (LXR pauses).
    #   * System.Runtime EventCounters @1s        - working-set + gc-committed.
    #   * LXRGC.Bench EventCounters   @1s          - the app's "operations" throughput.
    $providers = "Microsoft-Windows-DotNETRuntime:0x1:5," +
        "System.Runtime:0:1:EventCounterIntervalSec=1," +
        "LXRGC.Bench:0:1:EventCounterIntervalSec=1"
    $args = @("collect", "--process-id", "$TargetPid",
        "--providers", $providers,
        "--duration", $dur, "--output", $TraceOut)
    $outLog = Join-Path $rawDir "dotnet-trace-$TargetPid.log"
    return Start-Process -FilePath $dotnetTrace -ArgumentList $args -PassThru -NoNewWindow `
        -RedirectStandardOutput $outLog -RedirectStandardError "$outLog.err"
}

# Runs TraceAnalyzer offline on a captured .nettrace and fills $Series from the
# SINGLE trace session: GC-event series (pause_time_ms, heap_size, alloc_rate,
# gen0/1/2 collections) plus the EventCounter series (working_set, committed_bytes
# from System.Runtime; operations from LXRGC.Bench). Returns the unified per-pause
# sample array (ms), preferring LXR's own dynamic pause events, else built-in
# suspend-pair pauses.
function Merge-TraceSeries([hashtable]$Series, [string]$TraceOut, [string]$Label) {
    if (-not (Test-Path $TraceOut)) { return @() }
    $json = Join-Path $rawDir "$Label.tracestats.json"
    & $traceAnalyzerExe $TraceOut --json $json *> (Join-Path $rawDir "$Label.traceanalyzer.log") 2>&1
    if (-not (Test-Path $json)) { return @() }
    $a = Get-Content $json -Raw | ConvertFrom-Json
    if ($a.series) {
        foreach ($src in @("pause_time_ms", "heap_size", "alloc_rate", "working_set", "committed_bytes", "operations", "gen0_collections", "gen1_collections", "gen2_collections")) {
            $s = $a.series.$src
            if ($null -ne $s -and @($s).Count -gt 0) {
                $Series[$src] = @($s | ForEach-Object { @{ T = [double]$_.T; V = [double]$_.V } })
            }
        }
    }
    return @($a.pauseSamplesMs | Where-Object { $_ -ne $null } | ForEach-Object { [double]$_ })
}
# Runs a self-terminating benchmark process (console apps that stop themselves
# after their internal duration) under a single out-of-process dotnet-trace
# session. NO dotnet-counters: a second concurrent EventPipe client intermittently
# deadlocks the target at attach (which then hangs the whole suite). All series -
# pauses, heap, alloc, working-set, committed, operations - come from the trace.
# A watchdog kills a hung app + trace so one bad run can't freeze the suite.
function Invoke-MonitoredRun {
    param(
        [string]$ExePath,
        [string]$WorkingDirectory,
        [string]$Arguments,
        [hashtable]$ExtraEnv,
        [string[]]$RemoveEnvKeys,
        [string]$CsvBasePath,
        [int]$AttachDelayMs = 1500,
        [string]$TraceOut,
        [int]$TraceDurationSeconds = 0,
        [string]$Label = "run"
    )

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $ExePath
    $psi.WorkingDirectory = $WorkingDirectory
    $psi.Arguments = $Arguments
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    foreach ($k in $RemoveEnvKeys) { $psi.EnvironmentVariables.Remove($k) | Out-Null }
    foreach ($k in $ExtraEnv.Keys) { $psi.EnvironmentVariables[$k] = $ExtraEnv[$k] }

    $proc = [System.Diagnostics.Process]::Start($psi)
    $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
    $stderrTask = $proc.StandardError.ReadToEndAsync()
    Start-Sleep -Milliseconds $AttachDelayMs

    # Out-of-process event capture (authoritative, single session). Stop it a few
    # seconds before the app exits so dotnet-trace performs a clean rundown ->
    # complete .nettrace.
    $traceProc = $null
    if ($TraceOut -and $TraceDurationSeconds -gt 0 -and -not $proc.HasExited) {
        $traceSecs = [Math]::Max(1, $TraceDurationSeconds - 3)
        $traceProc = Start-TraceCollector -TargetPid $proc.Id -TraceOut $TraceOut -DurationSeconds $traceSecs
    }

    # Watchdog: allow the app its full duration plus generous slack, then force it
    # (and the trace) down so a hung run can't block the whole suite.
    $watchdogMs = ([Math]::Max(30, $TraceDurationSeconds) + 60) * 1000
    if (-not $proc.WaitForExit($watchdogMs)) {
        Write-Warning "[$Label] app did not exit within $([int]($watchdogMs/1000))s - killing (watchdog)."
        try { if ($traceProc -and -not $traceProc.HasExited) { $traceProc.Kill($true) } } catch {}
        try { if (-not $proc.HasExited) { $proc.Kill($true) } } catch {}
        $proc.WaitForExit(10000) | Out-Null
    }
    if ($traceProc -and -not $traceProc.HasExited) { $traceProc.WaitForExit(30000) | Out-Null }
    $stdout = $stdoutTask.Result
    $stderr = $stderrTask.Result

    # Every series is event-sourced; Merge-TraceSeries fills the (initially empty)
    # hashtable from the single trace and returns the per-pause sample array.
    $series = @{}
    $pauseSamplesMs = @()
    if ($TraceOut) { $pauseSamplesMs = Merge-TraceSeries -Series $series -TraceOut $TraceOut -Label $Label }

    return [pscustomobject]@{
        StdOut = $stdout
        StdErr = $stderr
        Series = $series
        PauseSamplesMs = $pauseSamplesMs
        ExitCode = $proc.ExitCode
    }
}

# Monitors a long-running HTTP server process for scenarios where the
# harness (not the target app) controls when the run ends. A single
# out-of-process dotnet-trace session (started with --duration = RunSeconds+slack)
# is the timing anchor: it auto-stops and flushes a complete .nettrace on its own
# after the fixed span, regardless of whether the server is still alive, and
# carries every series (pauses/heap/alloc/working-set/committed). NO dotnet-counters
# (a second concurrent EventPipe client intermittently deadlocks the target). The
# server is force-killed only AFTER the trace has exited.
#
# Load is driven by a background job that repeatedly POSTs small inference
# requests and records one completed-request count per wall-clock second;
# that per-second count is returned as a synthetic "operations" series in
# exactly the {T=seconds; V=count} shape the rest of the pipeline expects.
function Invoke-MonitoredServerRun {
    param(
        [string]$ExePath,
        [string]$WorkingDirectory,
        [string]$Arguments,
        [hashtable]$ExtraEnv,
        [string[]]$RemoveEnvKeys,
        [string]$CsvBasePath,
        [string]$ReadyUrl,
        [string]$RequestUrl,
        [string]$RequestBodyJson,
        [int]$RunSeconds,
        [int]$ReadyTimeoutSeconds = 90,
        [string]$TraceOut,
        [string]$Label = "run"
    )

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $ExePath
    $psi.WorkingDirectory = $WorkingDirectory
    $psi.Arguments = $Arguments
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    foreach ($k in $RemoveEnvKeys) { $psi.EnvironmentVariables.Remove($k) | Out-Null }
    foreach ($k in $ExtraEnv.Keys) { $psi.EnvironmentVariables[$k] = $ExtraEnv[$k] }

    $proc = [System.Diagnostics.Process]::Start($psi)
    $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
    $stderrTask = $proc.StandardError.ReadToEndAsync()

    # Wait for the server to actually accept requests (model load + warm-up
    # can take several seconds) before starting the timed measurement window.
    $ready = $false
    $readyDeadline = (Get-Date).AddSeconds($ReadyTimeoutSeconds)
    while ((Get-Date) -lt $readyDeadline) {
        if ($proc.HasExited) { break }
        try {
            Invoke-RestMethod -Uri $ReadyUrl -Method Post -Body $RequestBodyJson -ContentType "application/json" -TimeoutSec 10 | Out-Null
            $ready = $true
            break
        }
        catch { Start-Sleep -Milliseconds 500 }
    }
    if (-not $ready) {
        if (-not $proc.HasExited) { $proc.Kill() }
        throw "Server at $ReadyUrl did not become ready within $ReadyTimeoutSeconds s (stdout: $($stdoutTask.Result); stderr: $($stderrTask.Result))"
    }

    # Background load-driver job: sequential requests (dotLLM processes them
    # one at a time anyway), recording a completed-request count per second.
    $job = Start-Job -ScriptBlock {
        param($Url, $BodyJson, $DurationSec)
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $perSecond = @{}
        while ($sw.Elapsed.TotalSeconds -lt $DurationSec) {
            try {
                Invoke-RestMethod -Uri $Url -Method Post -Body $BodyJson -ContentType "application/json" -TimeoutSec 30 | Out-Null
                $sec = [int][Math]::Floor($sw.Elapsed.TotalSeconds)
                if ($perSecond.ContainsKey($sec)) { $perSecond[$sec] = $perSecond[$sec] + 1 } else { $perSecond[$sec] = 1 }
            }
            catch { Start-Sleep -Milliseconds 200 }
        }
        return $perSecond
    } -ArgumentList $RequestUrl, $RequestBodyJson, $RunSeconds

    # The dotnet-trace session (started with --duration) is the timing anchor: it
    # stops and flushes a complete .nettrace on its own after the fixed span,
    # independent of the server's lifetime. Slack over the load-driver duration so
    # the last few seconds of load are fully captured.
    $traceProc = $null
    if ($TraceOut) {
        $traceProc = Start-TraceCollector -TargetPid $proc.Id -TraceOut $TraceOut -DurationSeconds ($RunSeconds + 5)
    }
    if ($traceProc) {
        if (-not $traceProc.WaitForExit(($RunSeconds + 90) * 1000)) {
            Write-Warning "[$Label] dotnet-trace did not finish in time - killing (watchdog)."
            try { $traceProc.Kill($true) } catch {}
        }
    }
    else {
        Start-Sleep -Seconds ($RunSeconds + 5)
    }

    # The trace has now exited on its own - safe to reap the load-driver job and
    # finally kill the server.
    Wait-Job -Job $job -Timeout 30 | Out-Null
    $perSecondCounts = Receive-Job -Job $job -ErrorAction SilentlyContinue
    Remove-Job -Job $job -Force -ErrorAction SilentlyContinue

    if (-not $proc.HasExited) { $proc.Kill() }
    $proc.WaitForExit(15000) | Out-Null

    # Every non-synthetic series is event-sourced via the trace.
    $series = @{}

    # Build a synthetic "operations" series (requests completed per second)
    # in the same {T=seconds-since-start; V=value} shape used elsewhere.
    $opsSeries = @()
    if ($perSecondCounts) {
        foreach ($sec in ($perSecondCounts.Keys | Sort-Object)) {
            $opsSeries += @{ T = [double]$sec; V = [double]$perSecondCounts[$sec] }
        }
    }
    $totalOps = ($perSecondCounts.Values | Measure-Object -Sum).Sum
    if (-not $totalOps) { $totalOps = 0 }

    # Overlay event-sourced series (working_set/committed/heap/pauses/...) + collect
    # the authoritative per-pause samples. Set the synthetic operations LAST so the
    # load-driver count wins over any app-emitted operations EventCounter.
    $pauseSamplesMs = @()
    if ($TraceOut) { $pauseSamplesMs = Merge-TraceSeries -Series $series -TraceOut $TraceOut -Label $Label }
    $series["operations"] = $opsSeries

    return [pscustomobject]@{
        Series   = $series
        TotalOps = $totalOps
        PauseSamplesMs = $pauseSamplesMs
    }
}

function Get-ResultLineJson([string]$StdOut) {
    $line = ($StdOut -split "`n") | Where-Object { $_ -like "##RESULT##*" } | Select-Object -Last 1
    if (-not $line) { return $null }
    return ($line.Trim().Substring("##RESULT##".Length) | ConvertFrom-Json)
}

function Get-GcPerfSimStats([string]$StdOut) {
    function Num($name) {
        $m = [regex]::Match($StdOut, "$name`:\s*([0-9.]+)")
        if ($m.Success) { return [double]$m.Groups[1].Value }
        return 0
    }
    $collMatch = [regex]::Match($StdOut, "collection_counts:\s*\[([0-9,\s]+)\]")
    $gen0 = 0; $gen1 = 0; $gen2 = 0
    if ($collMatch.Success) {
        $parts = $collMatch.Groups[1].Value -split "," | ForEach-Object { $_.Trim() }
        if ($parts.Count -ge 3) { $gen0 = [int]$parts[0]; $gen1 = [int]$parts[1]; $gen2 = [int]$parts[2] }
    }
    return [pscustomobject]@{
        SohAllocatedBytes    = Num "sohAllocatedBytes"
        LohAllocatedBytes    = Num "lohAllocatedBytes"
        PohAllocatedBytes    = Num "pohAllocatedBytes"
        SecondsTaken         = Num "seconds_taken"
        Gen0Collections      = $gen0
        Gen1Collections      = $gen1
        Gen2Collections      = $gen2
        FinalTotalMemoryBytes = Num "final_total_memory_bytes"
        FinalHeapSizeBytes   = Num "final_heap_size_bytes"
        FinalFragmentationBytes = Num "final_fragmentation_bytes"
    }
}

# --- Main run loop ---------------------------------------------------------

$allRuns = @()
$runIndex = 0
$totalRuns = $Scenarios.Count * $GcModes.Count

foreach ($scenarioId in $Scenarios) {
    $scenario = $scenarioMap[$scenarioId]
    if (-not $scenario) { throw "Unknown scenario '$scenarioId'" }

    foreach ($gcModeId in $GcModes) {
        $gcMode = $gcModeMap[$gcModeId]
        if (-not $gcMode) { throw "Unknown GC mode '$gcModeId'" }

        $runIndex++
        $label = "$($scenario.Id)_$($gcMode.Id)"
        Write-Host "`n=== [$runIndex/$totalRuns] $($scenario.DisplayName) / $($gcMode.DisplayName) ===" -ForegroundColor Cyan

        $csvBase = Join-Path $rawDir $label
        $runStart = Get-Date

        # Authoritative per-pause STW samples (ms) come from an OUT-OF-PROCESS
        # dotnet-trace capture, reconstructed offline by TraceAnalyzer from the CLR
        # GC EventPipe stream (built-in GCs: GCSuspendEE*; LXRGC: its own GCDynamic
        # pause events). No in-process collector, no custom pause log. Reset per run
        # so a prior run's samples never leak.
        $pauseSamplesMs = $null
        $traceOut = Join-Path (Convert-Path $rawDir) "$label.nettrace"

        try {
        switch ($scenario.Kind) {
            "console" {
                $publishDir = Join-Path $root "samples\ConsoleApp\publish"
                $env2 = @{} + $gcMode.Env
                $run = Invoke-MonitoredRun -ExePath (Join-Path $publishDir "ConsoleApp.exe") -WorkingDirectory $publishDir `
                    -Arguments "$DurationSeconds $label" -ExtraEnv $env2 -RemoveEnvKeys $gcMode.RemoveEnv -CsvBasePath $csvBase `
                    -TraceOut $traceOut -TraceDurationSeconds $DurationSeconds -Label $label
                $resultJson = Get-ResultLineJson $run.StdOut
                if (-not $resultJson) { Write-Host $run.StdOut; Write-Host $run.StdErr; throw "No ##RESULT## for $label" }
                $pauseSamplesMs = @($run.PauseSamplesMs | Where-Object { $_ -ne $null })
                $summary = [ordered]@{
                    OperationsTotal      = $resultJson.Operations
                    OpsPerSecondOverall  = $resultJson.OpsPerSecond
                    TotalAllocatedBytes  = $resultJson.TotalAllocatedBytes
                    Gen0Collections      = $resultJson.Gen0Collections
                    Gen1Collections      = $resultJson.Gen1Collections
                    Gen2Collections      = $resultJson.Gen2Collections
                    WorkingSetBytes      = $resultJson.WorkingSetBytes
                    PeakWorkingSetBytes  = $resultJson.PeakWorkingSetBytes
                    HeapSizeBytes        = $resultJson.HeapSizeBytes
                    TotalCommittedBytes  = $resultJson.TotalCommittedBytes
                    GcName               = $resultJson.GcName
                }
                $durationActual = $resultJson.DurationSeconds
            }
            "webapi" {
                $publishDir = Join-Path $root "samples\WebApi\publish"
                $env2 = @{} + $gcMode.Env
                $env2["LXRGC_BENCH_DURATION_SECONDS"] = "$DurationSeconds"
                $env2["LXRGC_BENCH_LABEL"] = $label
                $run = Invoke-MonitoredRun -ExePath (Join-Path $publishDir "WebApi.exe") -WorkingDirectory $publishDir `
                    -Arguments "" -ExtraEnv $env2 -RemoveEnvKeys $gcMode.RemoveEnv -CsvBasePath $csvBase -AttachDelayMs 3000 `
                    -TraceOut $traceOut -TraceDurationSeconds $DurationSeconds -Label $label
                $resultJson = Get-ResultLineJson $run.StdOut
                if (-not $resultJson) { Write-Host $run.StdOut; Write-Host $run.StdErr; throw "No ##RESULT## for $label" }
                $pauseSamplesMs = @($run.PauseSamplesMs | Where-Object { $_ -ne $null })
                $summary = [ordered]@{
                    OperationsTotal      = $resultJson.Operations
                    OpsPerSecondOverall  = $resultJson.OpsPerSecond
                    TotalAllocatedBytes  = $resultJson.TotalAllocatedBytes
                    Gen0Collections      = $resultJson.Gen0Collections
                    Gen1Collections      = $resultJson.Gen1Collections
                    Gen2Collections      = $resultJson.Gen2Collections
                    WorkingSetBytes      = $resultJson.WorkingSetBytes
                    PeakWorkingSetBytes  = $resultJson.PeakWorkingSetBytes
                    HeapSizeBytes        = $resultJson.HeapSizeBytes
                    TotalCommittedBytes  = $resultJson.TotalCommittedBytes
                    GcName               = $resultJson.GcName
                    Errors               = $resultJson.Errors
                    TotalPauseTimeMs     = $resultJson.TotalPauseTimeMs
                    MaxPauseTimeMs       = $resultJson.MaxPauseTimeMs
                }
                $durationActual = $resultJson.DurationSeconds
            }
            "gcperfsim" {
                $publishDir = Join-Path $root "samples\GCPerfSim\publish"
                $tagb = [Math]::Round($scenario.BaseTagbFor600s * ($DurationSeconds / 600.0), 3)
                $fullArgs = "$($scenario.Args) -tagb $tagb"
                # gcperfsim-mt-throughput (and its -moderate variant) are
                # short (single-digit-to-tens-of-seconds) GC-bound bursts
                # rather than ~600s runs like the other GCPerfSim scenarios,
                # so the default 1500ms attach delay can eat too much of the
                # run for dotnet-counters to capture any samples (especially
                # under Server GC / LXRGC, the fastest modes here) - use a
                # much smaller attach delay.
                $attachDelayMs = if ($scenario.Id -in @("gcperfsim-mt-throughput", "gcperfsim-mt-throughput-moderate")) { 300 } else { 1500 }
                $run = Invoke-MonitoredRun -ExePath (Join-Path $publishDir "GCPerfSim.exe") -WorkingDirectory $publishDir `
                    -Arguments $fullArgs -ExtraEnv $gcMode.Env -RemoveEnvKeys $gcMode.RemoveEnv -CsvBasePath $csvBase -AttachDelayMs $attachDelayMs `
                    -TraceOut $traceOut -TraceDurationSeconds $DurationSeconds -Label $label
                $pauseSamplesMs = @($run.PauseSamplesMs | Where-Object { $_ -ne $null })
                $stats = Get-GcPerfSimStats $run.StdOut
                $totalAlloc = $stats.SohAllocatedBytes + $stats.LohAllocatedBytes + $stats.PohAllocatedBytes
                $summary = [ordered]@{
                    OperationsTotal      = $null
                    OpsPerSecondOverall  = $(if ($stats.SecondsTaken -gt 0) { $totalAlloc / $stats.SecondsTaken / 1MB } else { 0 }) # MB/s alloc throughput proxy
                    TotalAllocatedBytes  = $totalAlloc
                    Gen0Collections      = $stats.Gen0Collections
                    Gen1Collections      = $stats.Gen1Collections
                    Gen2Collections      = $stats.Gen2Collections
                    WorkingSetBytes      = $null
                    PeakWorkingSetBytes  = $null
                    HeapSizeBytes        = $stats.FinalHeapSizeBytes
                    TotalCommittedBytes  = $null
                    GcName               = $gcMode.DisplayName
                    TagbUsed             = $tagb
                }
                $durationActual = $stats.SecondsTaken
                if ($run.Series.Count -eq 0) { Write-Host $run.StdOut; Write-Host $run.StdErr; throw "No counters captured for $label" }
            }
            "zeroalloc" {
                $publishDir = Join-Path $root "samples\ZeroAllocApp\publish"
                $run = Invoke-MonitoredRun -ExePath (Join-Path $publishDir "ZeroAllocApp.exe") -WorkingDirectory $publishDir `
                    -Arguments "$DurationSeconds $label" -ExtraEnv $gcMode.Env -RemoveEnvKeys $gcMode.RemoveEnv -CsvBasePath $csvBase `
                    -TraceOut $traceOut -TraceDurationSeconds $DurationSeconds -Label $label
                $pauseSamplesMs = @($run.PauseSamplesMs | Where-Object { $_ -ne $null })
                $resultJson = Get-ResultLineJson $run.StdOut
                if (-not $resultJson) { Write-Host $run.StdOut; Write-Host $run.StdErr; throw "No ##RESULT## for $label" }
                $summary = [ordered]@{
                    OperationsTotal      = $resultJson.Operations
                    OpsPerSecondOverall  = $resultJson.OpsPerSecond
                    TotalAllocatedBytes  = $resultJson.TotalAllocatedBytes
                    BytesAllocatedThisThreadDuringRun = $resultJson.BytesAllocatedThisThreadDuringRun
                    Gen0Collections      = $resultJson.Gen0Collections
                    Gen1Collections      = $resultJson.Gen1Collections
                    Gen2Collections      = $resultJson.Gen2Collections
                    WorkingSetBytes      = $resultJson.WorkingSetBytes
                    PeakWorkingSetBytes  = $resultJson.PeakWorkingSetBytes
                    HeapSizeBytes        = $resultJson.HeapSizeBytes
                    TotalCommittedBytes  = $resultJson.TotalCommittedBytes
                    GcName               = $resultJson.GcName
                }
                $durationActual = $resultJson.DurationSeconds
            }
            "dotllm-serve" {
                $reqBody = '{"model":"SmolLM-135M.Q4_K_M.gguf","prompt":"The capital of France is","max_tokens":16}'
                $baseUrl = "http://localhost:$DotLlmPort/v1/completions"
                $env2 = @{} + $gcMode.Env
                $serveArgs = "serve `"$DotLlmModelFile`" --port $DotLlmPort --no-browser --no-ui"
                if ($dotLlmUseAssembledRoot) {
                    $exePath = $rootDotnet
                    $serveArgs = "`"$($dotLlmAppDll.FullName)`" $serveArgs"
                    $env2["DOTNET_ROOT"] = $DotLlmDotnetRoot
                    $env2["DOTNET_ROLL_FORWARD"] = "LatestMajor"
                }
                else { $exePath = $dotLlmExe }
                $svrRun = Invoke-MonitoredServerRun -ExePath $exePath -WorkingDirectory $root `
                    -Arguments $serveArgs `
                    -ExtraEnv $env2 -RemoveEnvKeys $gcMode.RemoveEnv -CsvBasePath $csvBase `
                    -ReadyUrl $baseUrl -RequestUrl $baseUrl -RequestBodyJson $reqBody -RunSeconds $DurationSeconds `
                    -TraceOut $traceOut -Label $label
                $pauseSamplesMs = @($svrRun.PauseSamplesMs | Where-Object { $_ -ne $null })
                $run = [pscustomobject]@{ Series = $svrRun.Series }
                $summary = [ordered]@{
                    OperationsTotal      = $svrRun.TotalOps
                    OpsPerSecondOverall  = $(if ($DurationSeconds -gt 0) { $svrRun.TotalOps / $DurationSeconds } else { 0 })
                    TotalAllocatedBytes  = $null
                    Gen0Collections      = $null
                    Gen1Collections      = $null
                    Gen2Collections      = $null
                    WorkingSetBytes      = $null
                    PeakWorkingSetBytes  = $null
                    HeapSizeBytes        = $null
                    TotalCommittedBytes  = $null
                    GcName               = $gcMode.DisplayName
                }
                $durationActual = $DurationSeconds
            }
            "dotllm-serve-large" {
                # A few-hundred-token prompt (~300 tokens as tokenized by the
                # model) about GC generations, followed by a short question -
                # long enough prefill to meaningfully exercise the model's
                # attention/KV-cache path over the tiny short-prompt scenario
                # above, while still completing in well under a minute per
                # request on CPU so a single benchmark run stays practical.
                $longPrompt = @'
You are a helpful assistant. Consider the following technical passage about garbage collection in modern managed runtimes, then answer the question at the end.

Garbage collection (GC) is an automatic memory management technique used by many programming language runtimes, including the Common Language Runtime (CLR) that powers .NET. The core idea is to relieve developers from the burden of manually tracking and freeing memory. Instead, the runtime periodically identifies objects that are no longer reachable from any root reference - such as local variables, static fields, or CPU registers - and reclaims the memory they occupy. Generational garbage collectors, like the one used in CoreCLR, divide the managed heap into multiple generations based on object lifetime. Generation 0 contains newly allocated objects. Generation 1 acts as a buffer between short-lived and long-lived objects. Generation 2 holds long-lived objects. The Large Object Heap holds objects larger than 85,000 bytes. Because most objects die young, this generational hypothesis lets a collector focus most of its work on generation 0, dramatically reducing average pause times. However, generation 2 collections, which must trace the entire live object graph reachable from the root set, can still take a long time when the retained heap is large. This is why long-running services with big in-memory caches sometimes see occasional, but very noticeable, garbage collection pauses.

Question: Summarize the tradeoff between generation 0 and generation 2 collections in one sentence.
'@
                $reqBody = (@{ model = "qwen2.5-1.5b-instruct-q4_k_m.gguf"; prompt = $longPrompt; max_tokens = 64 } | ConvertTo-Json -Compress)
                $baseUrl = "http://localhost:$DotLlmLargePort/v1/completions"
                $env2 = @{} + $gcMode.Env
                $serveArgs = "serve `"$DotLlmLargeModelFile`" --port $DotLlmLargePort --no-browser --no-ui"
                if ($dotLlmUseAssembledRoot) {
                    $exePath = $rootDotnet
                    $serveArgs = "`"$($dotLlmAppDll.FullName)`" $serveArgs"
                    $env2["DOTNET_ROOT"] = $DotLlmDotnetRoot
                    $env2["DOTNET_ROLL_FORWARD"] = "LatestMajor"
                }
                else { $exePath = $dotLlmExe }
                $svrRun = Invoke-MonitoredServerRun -ExePath $exePath -WorkingDirectory $root `
                    -Arguments $serveArgs `
                    -ExtraEnv $env2 -RemoveEnvKeys $gcMode.RemoveEnv -CsvBasePath $csvBase `
                    -ReadyUrl $baseUrl -RequestUrl $baseUrl -RequestBodyJson $reqBody -RunSeconds $DurationSeconds -ReadyTimeoutSeconds 300 `
                    -TraceOut $traceOut -Label $label
                $pauseSamplesMs = @($svrRun.PauseSamplesMs | Where-Object { $_ -ne $null })
                $run = [pscustomobject]@{ Series = $svrRun.Series }
                $summary = [ordered]@{
                    OperationsTotal      = $svrRun.TotalOps
                    OpsPerSecondOverall  = $(if ($DurationSeconds -gt 0) { $svrRun.TotalOps / $DurationSeconds } else { 0 })
                    TotalAllocatedBytes  = $null
                    Gen0Collections      = $null
                    Gen1Collections      = $null
                    Gen2Collections      = $null
                    WorkingSetBytes      = $null
                    PeakWorkingSetBytes  = $null
                    HeapSizeBytes        = $null
                    TotalCommittedBytes  = $null
                    GcName               = $gcMode.DisplayName
                }
                $durationActual = $DurationSeconds
            }
            "growingcache" {
                $publishDir = Join-Path $root "samples\GrowingCacheApp\publish"
                $run = Invoke-MonitoredRun -ExePath (Join-Path $publishDir "GrowingCacheApp.exe") -WorkingDirectory $publishDir `
                    -Arguments "$DurationSeconds $label $($scenario.Args)" -ExtraEnv $gcMode.Env -RemoveEnvKeys $gcMode.RemoveEnv -CsvBasePath $csvBase -AttachDelayMs 3000 `
                    -TraceOut $traceOut -TraceDurationSeconds $DurationSeconds -Label $label
                $pauseSamplesMs = @($run.PauseSamplesMs | Where-Object { $_ -ne $null })
                $resultJson = Get-ResultLineJson $run.StdOut
                if (-not $resultJson) { Write-Host $run.StdOut; Write-Host $run.StdErr; throw "No ##RESULT## for $label" }
                $summary = [ordered]@{
                    OperationsTotal      = $resultJson.Operations
                    OpsPerSecondOverall  = $resultJson.OpsPerSecond
                    TotalAllocatedBytes  = $resultJson.TotalAllocatedBytes
                    Gen0Collections      = $resultJson.Gen0Collections
                    Gen1Collections      = $resultJson.Gen1Collections
                    Gen2Collections      = $resultJson.Gen2Collections
                    WorkingSetBytes      = $resultJson.WorkingSetBytes
                    PeakWorkingSetBytes  = $resultJson.PeakWorkingSetBytes
                    HeapSizeBytes        = $resultJson.HeapSizeBytes
                    TotalCommittedBytes  = $resultJson.TotalCommittedBytes
                    GcName               = $resultJson.GcName
                    FinalCacheEntryCount   = $resultJson.FinalCacheEntryCount
                    ObservedGen2Events     = $resultJson.ObservedGen2Events
                    ObservedGen2PauseAvgMs = $resultJson.ObservedGen2PauseAvgMs
                    ObservedGen2PauseMaxMs = $resultJson.ObservedGen2PauseMaxMs
                    ObservedGen2PauseMinMs = $resultJson.ObservedGen2PauseMinMs
                    ObservedGen2PauseSamplesMs = $resultJson.ObservedGen2PauseSamplesMs
                }
                $durationActual = $resultJson.DurationSeconds
            }
        }

        # Fill in allocation/collection-count summary fields from the raw
        # counters series for scenarios (server-based ones) that can't
        # self-report these via a ##RESULT## line.
        if ($null -eq $summary.TotalAllocatedBytes -and $run.Series["alloc_rate"]) {
            $allocRawVals = @($run.Series["alloc_rate"] | ForEach-Object { $_.V })
            if ($allocRawVals.Count -gt 0) { $summary.TotalAllocatedBytes = ($allocRawVals | Measure-Object -Sum).Sum }
        }
        if ($null -eq $summary.Gen0Collections -and $run.Series["gen0_collections"]) {
            $summary.Gen0Collections = [int](@($run.Series["gen0_collections"] | ForEach-Object { $_.V }) | Measure-Object -Sum).Sum
        }
        if ($null -eq $summary.Gen1Collections -and $run.Series["gen1_collections"]) {
            $summary.Gen1Collections = [int](@($run.Series["gen1_collections"] | ForEach-Object { $_.V }) | Measure-Object -Sum).Sum
        }
        if ($null -eq $summary.Gen2Collections -and $run.Series["gen2_collections"]) {
            $summary.Gen2Collections = [int](@($run.Series["gen2_collections"] | ForEach-Object { $_.V }) | Measure-Object -Sum).Sum
        }

        # Fill in working-set/committed-bytes summary from counters if the
        # scenario didn't already report it (GCPerfSim has no such field).
        if ($null -eq $summary.WorkingSetBytes -and $run.Series["working_set"]) {
            $wsVals = $run.Series["working_set"] | ForEach-Object { $_.V }
            if ($wsVals.Count -gt 0) { $summary.WorkingSetBytes = ($wsVals | Measure-Object -Maximum).Maximum; $summary.PeakWorkingSetBytes = $summary.WorkingSetBytes }
        }
        if ($null -eq $summary.TotalCommittedBytes -and $run.Series["committed_bytes"]) {
            $cbVals = $run.Series["committed_bytes"] | ForEach-Object { $_.V }
            if ($cbVals.Count -gt 0) { $summary.TotalCommittedBytes = ($cbVals | Measure-Object -Maximum).Maximum }
        }

        # Pause distribution comes entirely from the out-of-process event trace
        # ($pauseSamplesMs, reconstructed by TraceAnalyzer). "over time" plots each
        # real pause (ms) at its timestamp - no coarse 1Hz counter involved.
        $wsValsAll = @($run.Series["working_set"] | ForEach-Object { $_.V })
        $allocRateVals = @($run.Series["alloc_rate"] | ForEach-Object { $_.V / 1MB }) # MB/s (event-sourced)
        $opsVals = @($run.Series["operations"] | ForEach-Object { $_.V })

        $elapsedWall = (Get-Date) - $runStart
        Write-Host ("  done in {0:N0}s wall (bench duration {1:N1}s), gen0/1/2={2}/{3}/{4}" -f `
            $elapsedWall.TotalSeconds, $durationActual, $summary.Gen0Collections, $summary.Gen1Collections, $summary.Gen2Collections)

        # Authoritative, event-sourced per-pause STW distribution (out-of-process
        # dotnet-trace: built-in GCs via GCSuspendEE*, LXRGC via its own GCDynamic
        # pause events). This is the SINGLE pause metric - the old coarse 1Hz
        # dotnet-counters pause row has been removed.
        $pauseDistVals = @($pauseSamplesMs | Where-Object { $_ -ne $null } | ForEach-Object { [double]$_ })
        $pauseDistStats = if ($pauseDistVals.Count -gt 0) {
            [ordered]@{
                Avg   = ($pauseDistVals | Measure-Object -Average).Average
                P50   = Get-Percentile $pauseDistVals 50
                P90   = Get-Percentile $pauseDistVals 90
                P95   = Get-Percentile $pauseDistVals 95
                P99   = Get-Percentile $pauseDistVals 99
                Max   = ($pauseDistVals | Measure-Object -Maximum).Maximum
                Total = ($pauseDistVals | Measure-Object -Sum).Sum
                Count = $pauseDistVals.Count
            }
        } else { $null }

        $allRuns += [pscustomobject]@{
            ScenarioId      = $scenario.Id
            ScenarioName    = $scenario.DisplayName
            GcModeId        = $gcMode.Id
            GcModeName      = $gcMode.DisplayName
            DurationSeconds = $durationActual
            Summary         = $summary
            PauseDistMsStats  = $pauseDistStats
            PauseSource       = $(if ($pauseDistVals.Count -gt 0) { "dotnet-trace events" } else { "n/a" })
            WorkingSetStats   = (Get-Stats $wsValsAll)
            AllocRateMBStats  = (Get-Stats $allocRateVals)
            OpsPerSecStats    = (Get-Stats $opsVals)
            TimeSeries = [ordered]@{
                PauseTimeMs  = Get-Downsampled (@($run.Series["pause_time_ms"] | ForEach-Object { @{ T = $_.T; V = [Math]::Round($_.V, 3) } }))
                WorkingSetMB = Get-Downsampled (@($run.Series["working_set"] | ForEach-Object { @{ T = $_.T; V = [Math]::Round($_.V / 1MB, 2) } }))
                AllocRateMB  = Get-Downsampled (@($run.Series["alloc_rate"] | ForEach-Object { @{ T = $_.T; V = [Math]::Round($_.V / 1MB, 2) } }))
                OpsPerSec    = Get-Downsampled (@($run.Series["operations"] | ForEach-Object { @{ T = $_.T; V = [Math]::Round($_.V, 1) } }))
                Gen0PerSec   = Get-Downsampled (@($run.Series["gen0_collections"] | ForEach-Object { @{ T = $_.T; V = $_.V } }))
            }
        }

        # Save incrementally so a crash partway through doesn't lose earlier runs.
        $allRuns | ConvertTo-Json -Depth 8 | Set-Content -Path (Join-Path $OutDir "results-full.json") -Encoding UTF8
        # Regenerate the human-readable report after every scenario so results
        # are viewable as they go, not only at the end of the (multi-hour) suite.
        try {
            & "$root\generate-report.ps1" -ResultsJson (Join-Path $OutDir "results-full.json") -OutHtml (Join-Path $OutDir "report.html") | Out-Null
        } catch {
            Write-Host ("  (incremental report regen skipped: {0})" -f $_.Exception.Message) -ForegroundColor DarkYellow
        }
        }
        catch {
            Write-Host ("  !! run '{0}' failed and was skipped: {1}" -f $label, $_.Exception.Message) -ForegroundColor Red
            # Best-effort cleanup of any orphaned server/app process for this run.
            Get-Process dotnet -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "C:\temp\lxr-dotnetroot\*" } | ForEach-Object { try { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue } catch {} }
            continue
        }
    }
}

Write-Host "`nAll $totalRuns runs complete." -ForegroundColor Green
Write-Host "Wrote $(Join-Path $OutDir 'results-full.json')" -ForegroundColor Green

& "$root\generate-report.ps1" -ResultsJson (Join-Path $OutDir "results-full.json") -OutHtml (Join-Path $OutDir "report.html")
