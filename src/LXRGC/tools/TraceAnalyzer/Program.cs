using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using Microsoft.Diagnostics.Tracing;
using Microsoft.Diagnostics.Tracing.Parsers;
using Microsoft.Diagnostics.Tracing.Parsers.Clr;

// Offline analyzer for LXRGC / built-in-GC EventPipe (.nettrace) traces.
//
// LXR emits its OWN GC dynamic events (GCDynamicEvent, EventID 39) named
// "LXRGCPause" / "LXRGCPhase" with a NUL-terminated ASCII "key=val;..." payload
// (see LXRGCHeap.cpp LXREmitDynamicEvent). We reconstruct the complete LXR pause
// distribution + per-phase timings from those. For the built-in GCs (Workstation/
// Server) we reconstruct pauses out-of-process from the standard
// GCSuspendEEStart -> GCRestartEEStop pair, so all three GC modes are measured
// uniformly, outside the app, from the same kind of trace.
class Program
{
    static int Main(string[] args)
    {
        if (args.Length < 1)
        {
            Console.Error.WriteLine("usage: TraceAnalyzer <trace.nettrace> [--dump] [--json <out.json>]");
            return 2;
        }
        string path = args[0];
        bool dump = args.Contains("--dump");
        if (args.Contains("--events")) return DumpEvents(path);
        string jsonOut = null;
        int ji = Array.IndexOf(args, "--json");
        if (ji >= 0 && ji + 1 < args.Length) jsonOut = args[ji + 1];

        var lxrPauses = new List<(double t, string type, double ms)>();
        var phaseAgg = new Dictionary<string, (int count, double total, double max, int concurrent)>();
        var suspendPauses = new List<(double t, double ms)>();
        // Event-sourced time series (timestamp in seconds, value).
        var heapSizeSeries = new List<(double t, double bytes)>();   // GCHeapStats.TotalHeapSize
        var allocTicks = new List<(double t, double bytes)>();       // GCAllocationTick.AllocationAmount64
        var gcCollections = new List<(double t, int depth)>();       // GCStart depth (gen collected)
        // EventCounter-sourced series (same single EventPipe session as the GC
        // events, so no second diagnostic client): working set + GC-committed from
        // the System.Runtime provider, throughput from the app's LXRGC.Bench
        // IncrementingEventCounter. These have no GC *event*, so they ride the
        // built-in EventCounters mechanism over the same trace.
        var workingSetSeries = new List<(double t, double bytes)>();
        var committedSeries = new List<(double t, double bytes)>();
        var opsSeries = new List<(double t, double v)>();
        int lxrDynCount = 0;
        double? pendingSuspendStart = null;

        using (var source = new EventPipeEventSource(path))
        {
            var clr = new ClrTraceEventParser(source);
            clr.GCSuspendEEStart += (GCSuspendEETraceData d) => { pendingSuspendStart = d.TimeStampRelativeMSec; };
            clr.GCRestartEEStop += (GCNoUserDataTraceData d) =>
            {
                if (pendingSuspendStart.HasValue)
                {
                    suspendPauses.Add((d.TimeStampRelativeMSec / 1000.0, d.TimeStampRelativeMSec - pendingSuspendStart.Value));
                    pendingSuspendStart = null;
                }
            };
            // Heap size / alloc / collections: fired natively by Workstation/Server
            // and by LXR through the same event sink, so all three GC modes yield
            // these series uniformly and out-of-process.
            clr.GCHeapStats += d => heapSizeSeries.Add((d.TimeStampRelativeMSec / 1000.0, d.TotalHeapSize));
            clr.GCAllocationTick += d => allocTicks.Add((d.TimeStampRelativeMSec / 1000.0, d.AllocationAmount64));
            clr.GCStart += d => gcCollections.Add((d.TimeStampRelativeMSec / 1000.0, d.Depth));

            // EventCounters (working-set, gc-committed, operations) over the same
            // session. Each "EventCounters" event carries a nested {Payload:{Name,
            // CounterType, Mean|Increment, IntervalSec, ...}} dictionary.
            source.Dynamic.All += (TraceEvent e) =>
            {
                if (e.EventName != "EventCounters") return;
                if (e.ProviderName != "System.Runtime" && e.ProviderName != "LXRGC.Bench") return;
                try
                {
                    var outer = e.PayloadValue(0) as IDictionary<string, object>;
                    var inner = outer != null && outer.TryGetValue("Payload", out var pv)
                        ? pv as IDictionary<string, object> : null;
                    if (inner == null) return;
                    string name = inner.TryGetValue("Name", out var nO) ? Convert.ToString(nO) : null;
                    if (name == null) return;
                    double t = e.TimeStampRelativeMSec / 1000.0;
                    double GetD(string k) => inner.TryGetValue(k, out var v) && v != null
                        ? Convert.ToDouble(v, CultureInfo.InvariantCulture) : 0.0;
                    if (e.ProviderName == "System.Runtime")
                    {
                        if (name == "working-set") workingSetSeries.Add((t, GetD("Mean") * 1024.0 * 1024.0));
                        else if (name == "gc-committed") committedSeries.Add((t, GetD("Mean") * 1024.0 * 1024.0));
                    }
                    else if (e.ProviderName == "LXRGC.Bench" && name == "operations")
                    {
                        double interval = GetD("IntervalSec"); if (interval <= 0) interval = 1.0;
                        opsSeries.Add((t, GetD("Increment") / interval));
                    }
                }
                catch { }
            };

            source.AllEvents += (TraceEvent e) =>
            {
                if (e.ProviderGuid != ClrTraceEventParser.ProviderGuid || (int)e.ID != 39) return;
                lxrDynCount++;
                string payload = ReadAsciiPayload(e);
                if (payload == null) return;
                var kv = ParseKv(payload);
                if (kv.TryGetValue("type", out var ptype))
                {
                    if (kv.TryGetValue("micros", out var us) && double.TryParse(us, NumberStyles.Any, CultureInfo.InvariantCulture, out var m))
                        lxrPauses.Add((e.TimeStampRelativeMSec / 1000.0, ptype, m / 1000.0));
                }
                else if (kv.TryGetValue("phase", out var phase))
                {
                    if (kv.TryGetValue("micros", out var us) && double.TryParse(us, NumberStyles.Any, CultureInfo.InvariantCulture, out var m))
                    {
                        double ms = m / 1000.0;
                        int conc = kv.TryGetValue("concurrent", out var c) && c == "1" ? 1 : 0;
                        if (!phaseAgg.TryGetValue(phase, out var cur)) cur = (0, 0, 0, conc);
                        phaseAgg[phase] = (cur.count + 1, cur.total + ms, Math.Max(cur.max, ms), conc);
                    }
                }
                if (dump) Console.Error.WriteLine($"[dyn] {payload}");
            };
            source.Process();
        }

        Console.WriteLine($"LXR dynamic events (EventID 39): {lxrDynCount}");
        Console.WriteLine($"LXR pause events: {lxrPauses.Count}");
        foreach (var g in lxrPauses.GroupBy(p => p.type))
            Console.WriteLine($"  {g.Key,-12} n={g.Count(),-4} max={g.Max(x => x.ms):F2}ms total={g.Sum(x => x.ms):F2}ms");
        var suspendMs = suspendPauses.Select(p => p.ms).ToList();
        Console.WriteLine($"Built-in suspend pauses (GCSuspendEE->GCRestartEE): {suspendPauses.Count}");
        if (suspendMs.Count > 0)
            Console.WriteLine($"  max={suspendMs.Max():F2}ms p99={Pct(suspendMs, 99):F2}ms p50={Pct(suspendMs, 50):F2}ms total={suspendMs.Sum():F2}ms");
        Console.WriteLine($"Heap-size samples (GCHeapStats): {heapSizeSeries.Count}  Alloc ticks: {allocTicks.Count}  GCStarts: {gcCollections.Count}");
        Console.WriteLine($"EventCounters: working-set={workingSetSeries.Count} gc-committed={committedSeries.Count} operations={opsSeries.Count}");
        Console.WriteLine("LXR phases:");
        foreach (var kv in phaseAgg.OrderByDescending(k => k.Value.total))
            Console.WriteLine($"  {kv.Key,-22} n={kv.Value.count,-4} total={kv.Value.total:F2}ms max={kv.Value.max:F2}ms {(kv.Value.concurrent == 1 ? "(off-pause)" : "(STW)")}");

        if (jsonOut != null)
        {
            // Unified pause distribution: prefer LXR's own per-pause dynamic events
            // (complete + phase-attributed) when present, else the built-in GC's
            // suspend-pair pauses. Both are out-of-process, event-sourced.
            bool haveLxr = lxrPauses.Count > 0;
            var pauseTimed = haveLxr
                ? lxrPauses.Select(p => (p.t, p.ms)).ToList()
                : suspendPauses.ToList();
            var pauseMs = pauseTimed.Select(p => p.ms).ToList();
            string pauseSource = haveLxr ? "lxr-dynamic" : "suspend";

            // 1s-bucketed alloc rate (bytes/sec) and gen0/1/2 cumulative collection
            // counts, plus raw heap-size samples, as {t,v} time series.
            var sb = new StringBuilder();
            sb.Append("{");
            sb.Append("\"pauseSource\":\"").Append(pauseSource).Append("\",");
            sb.Append("\"pauseSamplesMs\":[").Append(string.Join(",", pauseMs.Select(v => v.ToString("F4", CultureInfo.InvariantCulture)))).Append("],");
            // Back-compat: LXR-only per-pause samples (phase-report consumes these).
            sb.Append("\"lxrPauseSamplesMs\":[").Append(string.Join(",", lxrPauses.Select(p => p.ms.ToString("F4", CultureInfo.InvariantCulture)))).Append("],");
            sb.Append("\"lxrPauseByType\":{");
            sb.Append(string.Join(",", lxrPauses.GroupBy(p => p.type).Select(g =>
                $"\"{g.Key}\":{{\"count\":{g.Count()},\"maxMs\":{g.Max(x => x.ms).ToString("F4", CultureInfo.InvariantCulture)},\"totalMs\":{g.Sum(x => x.ms).ToString("F4", CultureInfo.InvariantCulture)}}}")));
            sb.Append("},");
            sb.Append("\"suspendPauseSamplesMs\":[").Append(string.Join(",", suspendMs.Select(v => v.ToString("F4", CultureInfo.InvariantCulture)))).Append("],");
            sb.Append("\"phases\":{");
            sb.Append(string.Join(",", phaseAgg.Select(kv =>
                $"\"{kv.Key}\":{{\"count\":{kv.Value.count},\"totalMs\":{kv.Value.total.ToString("F4", CultureInfo.InvariantCulture)},\"maxMs\":{kv.Value.max.ToString("F4", CultureInfo.InvariantCulture)},\"concurrent\":{kv.Value.concurrent}}}")));
            sb.Append("},");
            // time series
            sb.Append("\"series\":{");
            sb.Append("\"pause_time_ms\":").Append(SeriesJson(pauseTimed)).Append(",");
            sb.Append("\"heap_size\":").Append(SeriesJson(heapSizeSeries.Select(h => (h.t, h.bytes)).ToList())).Append(",");
            sb.Append("\"alloc_rate\":").Append(SeriesJson(BucketRatePerSec(allocTicks))).Append(",");
            sb.Append("\"gen0_collections\":").Append(SeriesJson(CumulativeCollections(gcCollections, 0))).Append(",");
            sb.Append("\"gen1_collections\":").Append(SeriesJson(CumulativeCollections(gcCollections, 1))).Append(",");
            sb.Append("\"gen2_collections\":").Append(SeriesJson(CumulativeCollections(gcCollections, 2))).Append(",");
            sb.Append("\"working_set\":").Append(SeriesJson(workingSetSeries.Select(x => (x.t, x.bytes)).ToList())).Append(",");
            sb.Append("\"committed_bytes\":").Append(SeriesJson(committedSeries.Select(x => (x.t, x.bytes)).ToList())).Append(",");
            sb.Append("\"operations\":").Append(SeriesJson(opsSeries));
            sb.Append("}");
            sb.Append("}");
            System.IO.File.WriteAllText(jsonOut, sb.ToString());
            Console.WriteLine($"wrote {jsonOut}");
        }
        return 0;
    }

    // {t,v} JSON array, rounded.
    static string SeriesJson(List<(double t, double v)> s)
    {
        return "[" + string.Join(",", s.Select(p =>
            $"{{\"T\":{Math.Round(p.t, 1).ToString("F1", CultureInfo.InvariantCulture)},\"V\":{p.v.ToString("F3", CultureInfo.InvariantCulture)}}}")) + "]";
    }

    // Sum event amounts into 1-second buckets and express as a per-second rate
    // (bytes/sec), matching the shape dotnet-counters produced for alloc_rate.
    static List<(double t, double v)> BucketRatePerSec(List<(double t, double bytes)> events)
    {
        var byBucket = new SortedDictionary<int, double>();
        foreach (var (t, bytes) in events)
        {
            int b = (int)Math.Floor(t);
            byBucket[b] = byBucket.TryGetValue(b, out var cur) ? cur + bytes : bytes;
        }
        return byBucket.Select(kv => ((double)kv.Key, kv.Value)).ToList();
    }

    // One increment (V=1) per GC whose condemned generation == gen, at its
    // timestamp. Emitting increments (not a running total) matches the
    // dotnet-counters "collections" increment semantics the harness sums over.
    static List<(double t, double v)> CumulativeCollections(List<(double t, int depth)> starts, int gen)
    {
        return starts.Where(s => s.depth == gen).OrderBy(s => s.t)
                     .Select(s => (s.t, 1.0)).ToList();
    }

    // Recon: tally every event by provider/name and, for the GC events we care
    // about, print the available payload field names + a sample row. Used to
    // design the event-sourced time series (heap/committed/alloc/collections).
    static int DumpEvents(string path)
    {
        var counts = new Dictionary<string, int>();
        var fields = new Dictionary<string, string>();
        void Cap(string name, TraceEvent e)
        {
            counts[name] = counts.TryGetValue(name, out var c) ? c + 1 : 1;
            if (!fields.ContainsKey(name))
            {
                try { fields[name] = string.Join(", ", e.PayloadNames.Select(n => n + "=" + Convert.ToString(e.PayloadByName(n)))); }
                catch { fields[name] = "(payload read failed): " + string.Join(", ", e.PayloadNames); }
            }
        }
        using (var source = new EventPipeEventSource(path))
        {
            var clr = new ClrTraceEventParser(source);
            clr.GCHeapStats += d => Cap("GCHeapStats", d);
            clr.GCAllocationTick += d => Cap("GCAllocationTick", d);
            clr.GCStart += d => Cap("GCStart", d);
            clr.GCStop += d => Cap("GCEnd", d);
            clr.GCGlobalHeapHistory += d => Cap("GCGlobalHeapHistory", d);
            clr.GCPerHeapHistory += d => Cap("GCPerHeapHistory", d);
            clr.GCSuspendEEStart += d => Cap("GCSuspendEEStart", d);
            clr.GCRestartEEStop += d => Cap("GCRestartEEStop", d);
            source.Dynamic.All += d => {
                if (d.ProviderName == "System.Runtime" || d.ProviderName == "LXRGC.Bench" ||
                    d.ProviderName == "System.Diagnostics.Metrics")
                {
                    string nm = d.ProviderName + "/" + d.EventName;
                    Cap(nm, d);
                    if (d.EventName == "EventCounters")
                    {
                        try {
                            var outer = d.PayloadValue(0) as IDictionary<string, object>;
                            var inner = outer != null && outer.TryGetValue("Payload", out var pv)
                                ? pv as IDictionary<string, object> : null;
                            if (inner != null && inner.TryGetValue("Name", out var cn))
                                Cap("EC[" + d.ProviderName + "]:" + Convert.ToString(cn), d);
                        } catch { }
                    }
                }
            };
            source.Process();
        }
        Console.WriteLine("=== typed GC event counts ===");
        foreach (var kv in counts.OrderByDescending(k => k.Value))
            Console.WriteLine($"  {kv.Value,8}  {kv.Key}");
        Console.WriteLine("\n=== payload fields (first sample) ===");
        foreach (var kv in fields.OrderBy(k => k.Key))
            Console.WriteLine($"\n[{kv.Key}]\n   {kv.Value}");
        return 0;
    }

    static string ReadAsciiPayload(TraceEvent e)
    {
        byte[] blob;
        try { blob = e.EventData(); } catch { return null; }
        if (blob == null) return null;
        for (int i = 0; i < blob.Length; i++)
        {
            int run = 0, j = i;
            while (j < blob.Length && blob[j] >= 0x20 && blob[j] < 0x7f) { run++; j++; }
            if (run >= 4) return Encoding.ASCII.GetString(blob, i, j - i);
        }
        return null;
    }

    static Dictionary<string, string> ParseKv(string s)
    {
        var d = new Dictionary<string, string>();
        foreach (var part in s.Split(';'))
        {
            int eq = part.IndexOf('=');
            if (eq > 0) d[part.Substring(0, eq)] = part.Substring(eq + 1);
        }
        return d;
    }

    static double Pct(List<double> data, double p)
    {
        if (data.Count == 0) return 0;
        var sorted = data.OrderBy(x => x).ToList();
        double rank = (p / 100.0) * (sorted.Count - 1);
        int lo = (int)Math.Floor(rank), hi = (int)Math.Ceiling(rank);
        if (lo == hi) return sorted[lo];
        return sorted[lo] + (rank - lo) * (sorted[hi] - sorted[lo]);
    }
}
