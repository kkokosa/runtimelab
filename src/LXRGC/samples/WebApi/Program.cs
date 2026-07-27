// LXRGC benchmark ASP.NET Core (minimal API / Kestrel) app.
//
// Hosts a small set of endpoints that do realistic per-request allocation
// (JSON (de)serialization, string building, list/dictionary work), then
// drives itself with an in-process HTTP client for a fixed duration so the
// whole run (server + load generator) is a single, easily automated
// process. Reports the same BenchResult JSON shape as the console sample so
// both can be compared/plotted uniformly.

using System.Diagnostics;
using System.Diagnostics.Metrics;
using System.Runtime;
using System.Text.Json;

// Same custom Meter convention as ConsoleApp - lets `dotnet-counters collect`
// capture a uniform request-throughput time series across both sample apps.
var meter = new Meter("LXRGC.Bench");
var opsCounter = meter.CreateCounter<long>("operations", description: "Completed benchmark operations");

var builder = WebApplication.CreateBuilder(args);
builder.Logging.ClearProviders(); // keep stdout free for the ##RESULT## line
builder.WebHost.UseUrls("http://127.0.0.1:0"); // let Kestrel pick a free port
var app = builder.Build();

// Item G probe: when LXR_BIGARRAY_PROBE is set, allocate a single very large
// live reference array (> the collector's 64K-slot partition threshold) and keep
// mutating/reachable for the whole run, so the parallel big-array mark path
// (DrainDeferredBigArrays) is exercised on every trace. Held in a static field so
// it stays a GC root for the process lifetime.
BigArrayProbe.Start();

// Per-request handler: simulates a small JSON API endpoint that builds a
// response object, serializes it, and does a bit of string/list work -
// representative gen0-heavy allocation churn for a typical web API.
app.MapGet("/api/work", (int size) =>
{
    var items = new List<WorkItem>(size);
    for (int i = 0; i < size; i++)
    {
        items.Add(new WorkItem(i, $"item-{i}-{Guid.NewGuid():N}", i * 1.5));
    }
    var payload = new WorkResponse(items.Count, items.Sum(x => x.Value), items.Take(5).ToArray());
    return Results.Json(payload);
});

app.MapGet("/api/ping", () => "pong");

app.Lifetime.ApplicationStarted.Register(() =>
{
    // Kick off the self-driving load generator once Kestrel is actually listening.
    _ = Task.Run(() => RunBenchmarkAndExitAsync(app, opsCounter));
});

app.Run();

static async Task RunBenchmarkAndExitAsync(WebApplication app, Counter<long> opsCounter)
{
    int durationSeconds = int.TryParse(Environment.GetEnvironmentVariable("LXRGC_BENCH_DURATION_SECONDS"), out var d) ? d : 60;
    string label = Environment.GetEnvironmentVariable("LXRGC_BENCH_LABEL") ?? "run";

    string gcName = Environment.GetEnvironmentVariable("DOTNET_GCName")
        ?? Environment.GetEnvironmentVariable("COMPlus_GCName")
        ?? (GCSettings.IsServerGC ? "CoreCLR (Server)" : "CoreCLR (Workstation)");

    var addresses = app.Services.GetRequiredService<Microsoft.AspNetCore.Hosting.Server.IServer>()
        .Features.Get<Microsoft.AspNetCore.Hosting.Server.Features.IServerAddressesFeature>();
    string baseUrl = addresses?.Addresses.FirstOrDefault() ?? "http://127.0.0.1:5000";

    Console.WriteLine($"# LXRGC-bench WebApi starting: duration={durationSeconds}s label={label} url={baseUrl}");
    Console.WriteLine($"# GC.Name={gcName}");

    using var client = new HttpClient { BaseAddress = new Uri(baseUrl), Timeout = TimeSpan.FromSeconds(10) };

    // Warm up JIT / connection pool before measuring.
    await client.GetStringAsync("/api/ping");

    // Drive load with several concurrent worker loops rather than one
    // sequential loop. A single loop throttled via Task.Delay(1) tops out
    // around ~60 req/sec (bounded by Windows timer granularity, ~16ms),
    // which is too little traffic to generate meaningful GC pressure over a
    // multi-minute run. N concurrent workers (each still individually
    // throttled the same way) multiply aggregate throughput roughly by N
    // while keeping the per-worker allocation/memory-growth rate - and
    // therefore total run memory - predictable and bounded regardless of
    // which GC is loaded.
    // NOTE: default lowered from 8 to 4. LXRGC's collector is stop-the-world and
    // single-threaded (concurrent/incremental collection is future work, see
    // FEASIBILITY.md), so beyond ~4 concurrently-allocating request threads it
    // cannot keep pace with allocation and SuspendEE/RestartEE cycling starves
    // forward progress. 4 workers is still a realistic web load and is applied
    // uniformly to every GC mode, so the cross-GC comparison stays fair.
    int workerCount = int.TryParse(Environment.GetEnvironmentVariable("LXRGC_BENCH_WEBAPI_WORKERS"), out var wc) ? wc : 4;

    long requests = 0;
    long errors = 0;
    var sw = Stopwatch.StartNew();
    var deadline = TimeSpan.FromSeconds(durationSeconds);
    var proc = Process.GetCurrentProcess();

    async Task WorkerAsync(int workerId)
    {
        var rng = new Random(12345 + workerId);
        using var workerClient = new HttpClient { BaseAddress = new Uri(baseUrl), Timeout = TimeSpan.FromSeconds(10) };
        while (sw.Elapsed < deadline)
        {
            try
            {
                int size = rng.Next(5, 50);
                var resp = await workerClient.GetStringAsync($"/api/work?size={size}");
                Interlocked.Increment(ref requests);
                opsCounter.Add(1);
                if (resp.Length == -1) Console.WriteLine(resp); // never true; prevents dead-code elimination
            }
            catch
            {
                Interlocked.Increment(ref errors);
            }

            // Per-worker throttle (see comment above): keeps aggregate
            // memory growth bounded and predictable while still scaling
            // total throughput with workerCount.
            await Task.Delay(1);
        }
    }

    var workers = Enumerable.Range(0, workerCount).Select(WorkerAsync).ToArray();
    await Task.WhenAll(workers);

    sw.Stop();
    proc.Refresh();

    var result = new BenchResult
    {
        Label = label,
        DurationSeconds = sw.Elapsed.TotalSeconds,
        Operations = requests,
        OpsPerSecond = requests / sw.Elapsed.TotalSeconds,
        GcName = gcName,
        TotalAllocatedBytes = GC.GetTotalAllocatedBytes(precise: false),
        Gen0Collections = GC.CollectionCount(0),
        Gen1Collections = GC.CollectionCount(1),
        Gen2Collections = GC.CollectionCount(2),
        WorkingSetBytes = proc.WorkingSet64,
        PrivateBytes = proc.PrivateMemorySize64,
        PeakWorkingSetBytes = proc.PeakWorkingSet64,
        Errors = errors
    };

    var memInfo = GC.GetGCMemoryInfo();
    result.HeapSizeBytes = memInfo.HeapSizeBytes;
    result.TotalCommittedBytes = memInfo.TotalCommittedBytes;
    result.PauseTimePercentage = memInfo.PauseTimePercentage;
    // Honest pause-time-in-ms telemetry (the percentage alone hides how long
    // individual STW pauses actually are). TotalPauseTimeMs is cumulative wall
    // clock spent stopped; MaxPauseTimeMs is the single longest STW pause (the
    // headline latency figure for a low-pause collector). PauseDurations[1] is
    // the collector's max-pause counter (LXRGC); the built-in GCs only report the
    // latest GC's durations there, so MaxPauseTimeMs is meaningful for LXRGC and
    // best-effort otherwise.
    result.TotalPauseTimeMs = GC.GetTotalPauseDuration().TotalMilliseconds;
    var pd = memInfo.PauseDurations;
    result.MaxPauseTimeMs = pd.Length > 1 ? pd[1].TotalMilliseconds : (pd.Length > 0 ? pd[0].TotalMilliseconds : 0);

    string json = JsonSerializer.Serialize(result, new JsonSerializerOptions { WriteIndented = false });
    Console.WriteLine("##RESULT##" + json);

    await app.StopAsync();
    Environment.Exit(0);
}

internal record WorkItem(int Id, string Name, double Value);
internal record WorkResponse(int Count, double Sum, WorkItem[] Sample);

// Item G probe (see Program top). Keeps a huge live object[] rooted in a static
// field and a background thread that periodically stores new elements so the
// array stays referenced and dirty across every trace.
internal static class BigArrayProbe
{
    private static object[]? s_big;
    public static void Start()
    {
        if (Environment.GetEnvironmentVariable("LXR_BIGARRAY_PROBE") == null)
            return;
        int n = 100_000; // >> 64K partition threshold, but a modest 800KB array
        var big = new object[n];
        // Populate sparsely with live objects so the parallel chunk scan has real
        // non-null out-edges to follow, without a huge tiny-object burst.
        for (int i = 0; i < n; i += 16)
            big[i] = new object();
        s_big = big;
        Console.WriteLine($"# LXR_BIGARRAY_PROBE: allocated object[{n}] live ref array");
        var t = new Thread(() =>
        {
            long k = 0;
            while (true)
            {
                var a = s_big;
                if (a != null)
                    a[(int)(k % a.Length)] = new object();
                k += 997;
                Thread.Sleep(5);
            }
        });
        t.IsBackground = true;
        t.Start();
    }
}

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
    public double TotalPauseTimeMs { get; set; }
    public double MaxPauseTimeMs { get; set; }
    public long Errors { get; set; }
}
