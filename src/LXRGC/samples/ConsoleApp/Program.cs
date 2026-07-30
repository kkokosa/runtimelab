// LXRGC benchmark console app.
//
// Runs a mixed allocation workload (small short-lived objects, some medium
// "survivor-like" objects kept alive in a list, and occasional large object
// heap allocations) for a fixed duration, then reports throughput and GC
// counters to stdout as a single JSON line so the harness can parse it.

using System.Diagnostics;
using System.Diagnostics.Metrics;
using System.Diagnostics.Tracing;
using System.Runtime;
using System.Text.Json;

// Custom Meter so `dotnet-counters collect` can capture a uniform
// "operations" throughput time series (rate/sec) for this app, the same way
// it captures built-in GC counters like gen-0-gc-count or time-in-gc. This
// lets the benchmark harness derive a real throughput-over-time chart
// without parsing custom app output.
var meter = new Meter("LXRGC.Bench");
long opsForCounter = 0;
var opsCounter = meter.CreateCounter<long>("operations", description: "Completed benchmark operations");

int durationSeconds = args.Length > 0 && int.TryParse(args[0], out var d) ? d : 60;
string label = args.Length > 1 ? args[1] : "run";

// AppContext doesn't expose the resolved standalone GC name to managed code,
// so report the DOTNET_GCName knob directly (this is exactly how the bench
// harness selects LXRGC vs. the built-in GC in the first place).
string gcName = Environment.GetEnvironmentVariable("DOTNET_GCName")
    ?? Environment.GetEnvironmentVariable("COMPlus_GCName")
    ?? (GCSettings.IsServerGC ? "CoreCLR (Server)" : "CoreCLR (Workstation)");

Console.WriteLine($"# LXRGC-bench ConsoleApp starting: duration={durationSeconds}s label={label}");
Console.WriteLine($"# GC.Name={gcName}");

// Precise per-pause STW latency capture, unified across all three GCs (built-in
// via an in-process GC EventListener; LXRGC via its LXR_PAUSE_LOG file). Started
// before the workload so no pause is missed.
var pauseCollector = GcPauseCollector.Start();

var survivors = new List<byte[]>();
var rng = new Random(12345);

// Item G probe: when LXR_BIGARRAY_PROBE is set, allocate a single very large
// reference array (> the collector's 64K-slot partition threshold) full of live
// objects and keep mutating/reachable across the whole run, so the parallel
// mark path (DrainDeferredBigArrays) is exercised on every trace.
object[]? bigRefArray = null;
if (Environment.GetEnvironmentVariable("LXR_BIGARRAY_PROBE") != null)
{
    int n = 2_000_000; // 2M slots >> 64K threshold (~16MB of pointers)
    bigRefArray = new object[n];
    for (int i = 0; i < n; i++)
        bigRefArray[i] = new object();
    Console.WriteLine($"# LXR_BIGARRAY_PROBE: allocated object[{n}] live ref array");
}

long ops = 0;
var sw = Stopwatch.StartNew();
var deadline = TimeSpan.FromSeconds(durationSeconds);

var proc = Process.GetCurrentProcess();

while (sw.Elapsed < deadline)
{
    // Small short-lived allocations (typical gen0 churn).
    for (int i = 0; i < 200; i++)
    {
        var small = new byte[rng.Next(16, 256)];
        small[0] = (byte)i;
        ops++;
    }

    // Some strings / small objects too.
    for (int i = 0; i < 50; i++)
    {
        var s = new string('x', rng.Next(8, 64));
        ops++;
        if (s.Length == -1) Console.WriteLine(s); // never true; keeps JIT from eliding the alloc
    }

    // Occasionally keep something alive (simulates real app state growth).
    if (ops % 5000 == 0)
    {
        survivors.Add(new byte[rng.Next(1024, 4096)]);
    }

    // Occasional large object heap allocation.
    if (ops % 20000 == 0)
    {
        var large = new byte[rng.Next(90_000, 200_000)];
        large[0] = 1;
        ops++;
    }

    opsCounter.Add(ops - opsForCounter);
    opsForCounter = ops;

    // Throttle to a realistic sustained allocation rate. A GC that never
    // reclaims memory would otherwise commit many tens of GB per minute at
    // an unthrottled rate, which is unrepresentative of real workloads and
    // risks exhausting machine memory during a multi-minute comparison run.
    Thread.Sleep(1);

    // Keep the big-array probe live and mutating (new element stores keep it
    // referenced and dirty so every trace re-scans it).
    if (bigRefArray != null && (ops % 1000 == 0))
        bigRefArray[(int)((ops / 1000) % bigRefArray.Length)] = new object();
}
GC.KeepAlive(bigRefArray);

sw.Stop();
proc.Refresh();

var result = new BenchResult
{
    Label = label,
    DurationSeconds = sw.Elapsed.TotalSeconds,
    Operations = ops,
    OpsPerSecond = ops / sw.Elapsed.TotalSeconds,
    GcName = gcName,
    TotalAllocatedBytes = GC.GetTotalAllocatedBytes(precise: false),
    Gen0Collections = GC.CollectionCount(0),
    Gen1Collections = GC.CollectionCount(1),
    Gen2Collections = GC.CollectionCount(2),
    WorkingSetBytes = proc.WorkingSet64,
    PrivateBytes = proc.PrivateMemorySize64,
    PeakWorkingSetBytes = proc.PeakWorkingSet64,
    SurvivorListCount = survivors.Count
};

var memInfo = GC.GetGCMemoryInfo();
result.HeapSizeBytes = memInfo.HeapSizeBytes;
result.TotalCommittedBytes = memInfo.TotalCommittedBytes;
result.PauseTimePercentage = memInfo.PauseTimePercentage;

// Exact per-pause distribution (see GcPauseCollector). When available it is the
// authoritative STW latency metric (p50/p95/p99/max), replacing the coarse 1Hz
// PauseTimePercentage counter that misses LXR's sub-5ms pauses.
var pauses = pauseCollector.GetSamplesMs();
if (pauses.Count > 0)
    result.PauseSamplesMs = pauses;

string json = JsonSerializer.Serialize(result, new JsonSerializerOptions { WriteIndented = false });
Console.WriteLine("##RESULT##" + json);

GC.KeepAlive(survivors);

internal class BenchResult
{
    public string Label { get; set; } = "";
    public double DurationSeconds { get; set; }
    public long Operations { get; set; }
    public double OpsPerSecond { get; set; }
    public string GcName { get; set; } = "";
    public long TotalAllocatedBytes { get; set; }
    public int Gen0Collections { get; set; }
    public int Gen1Collections { get; set; }
    public int Gen2Collections { get; set; }
    public long WorkingSetBytes { get; set; }
    public long PrivateBytes { get; set; }
    public long PeakWorkingSetBytes { get; set; }
    public long HeapSizeBytes { get; set; }
    public long TotalCommittedBytes { get; set; }
    public double PauseTimePercentage { get; set; }
    public int SurvivorListCount { get; set; }
    public List<double>? PauseSamplesMs { get; set; }
}

// Precise per-pause STW latency capture, unified across all three GCs.
//
//  * Built-in Workstation/Server GC: hosts an in-process EventListener on the
//    runtime GC provider ("Microsoft-Windows-DotNETRuntime", GC keyword 0x1) and
//    pairs each GCSuspendEEBegin -> GCRestartEEEnd, timing the exact stop-the-world
//    window from the 100ns event timestamps.
//  * Standalone LXRGC: fires no ETW/EventPipe GC events, so the listener would see
//    nothing. Instead the native GC writes each QPC-timed pause to LXR_PAUSE_LOG
//    (env), which we read at the end. Not attaching an EventListener under LXR also
//    avoids the known EventPipe-attach-during-startup hang.
internal sealed class GcPauseCollector
{
    private readonly GcEventListener? _listener;
    private readonly string? _lxrPauseLog;

    private GcPauseCollector(GcEventListener? listener, string? lxrPauseLog)
    {
        _listener = listener;
        _lxrPauseLog = lxrPauseLog;
    }

    public static GcPauseCollector Start()
    {
        // LXRGC is selected via DOTNET_GCName; it self-reports pauses to a file.
        bool isLxr = !string.IsNullOrEmpty(Environment.GetEnvironmentVariable("DOTNET_GCName"));
        if (isLxr)
            return new GcPauseCollector(null, Environment.GetEnvironmentVariable("LXR_PAUSE_LOG"));
        return new GcPauseCollector(new GcEventListener(), null);
    }

    public List<double> GetSamplesMs()
    {
        if (_listener != null)
            return _listener.Snapshot();

        var list = new List<double>();
        try
        {
            if (!string.IsNullOrEmpty(_lxrPauseLog) && File.Exists(_lxrPauseLog))
            {
                using var fs = new FileStream(_lxrPauseLog, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
                using var sr = new StreamReader(fs);
                string? line;
                while ((line = sr.ReadLine()) != null)
                {
                    int comma = line.LastIndexOf(',');
                    if (comma < 0) continue;
                    if (long.TryParse(line.AsSpan(comma + 1), out long micros) && micros > 0)
                        list.Add(micros / 1000.0);
                }
            }
        }
        catch { /* best-effort: a missing/locked log just yields no samples */ }
        return list;
    }
}

internal sealed class GcEventListener : EventListener
{
    private const string RuntimeProvider = "Microsoft-Windows-DotNETRuntime";
    private const EventKeywords GCKeyword = (EventKeywords)0x1;
    private readonly List<double> _pausesMs = new();
    private readonly object _lock = new();
    private DateTime _suspendStart;
    private bool _inSuspend;

    protected override void OnEventSourceCreated(EventSource source)
    {
        if (source.Name == RuntimeProvider)
            EnableEvents(source, EventLevel.Informational, GCKeyword);
    }

    protected override void OnEventWritten(EventWrittenEventArgs e)
    {
        switch (e.EventName)
        {
            case "GCSuspendEEBegin_V1":
            case "GCSuspendEEBegin":
                lock (_lock) { _suspendStart = e.TimeStamp; _inSuspend = true; }
                break;
            case "GCRestartEEEnd_V1":
            case "GCRestartEEEnd":
                lock (_lock)
                {
                    if (_inSuspend)
                    {
                        double ms = (e.TimeStamp - _suspendStart).TotalMilliseconds;
                        if (ms >= 0) _pausesMs.Add(ms);
                        _inSuspend = false;
                    }
                }
                break;
        }
    }

    public List<double> Snapshot()
    {
        lock (_lock) return new List<double>(_pausesMs);
    }
}
