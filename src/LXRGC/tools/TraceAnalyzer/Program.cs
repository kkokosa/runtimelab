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
        string jsonOut = null;
        int ji = Array.IndexOf(args, "--json");
        if (ji >= 0 && ji + 1 < args.Length) jsonOut = args[ji + 1];

        var lxrPauses = new List<(string type, double ms)>();
        var phaseAgg = new Dictionary<string, (int count, double total, double max, int concurrent)>();
        var suspendPauses = new List<double>();
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
                    suspendPauses.Add(d.TimeStampRelativeMSec - pendingSuspendStart.Value);
                    pendingSuspendStart = null;
                }
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
                        lxrPauses.Add((ptype, m / 1000.0));
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
        Console.WriteLine($"Built-in suspend pauses (GCSuspendEE->GCRestartEE): {suspendPauses.Count}");
        if (suspendPauses.Count > 0)
            Console.WriteLine($"  max={suspendPauses.Max():F2}ms p99={Pct(suspendPauses, 99):F2}ms p50={Pct(suspendPauses, 50):F2}ms total={suspendPauses.Sum():F2}ms");
        Console.WriteLine("LXR phases:");
        foreach (var kv in phaseAgg.OrderByDescending(k => k.Value.total))
            Console.WriteLine($"  {kv.Key,-22} n={kv.Value.count,-4} total={kv.Value.total:F2}ms max={kv.Value.max:F2}ms {(kv.Value.concurrent == 1 ? "(off-pause)" : "(STW)")}");

        if (jsonOut != null)
        {
            var allLxrMs = lxrPauses.Select(p => p.ms).ToList();
            var sb = new StringBuilder();
            sb.Append("{");
            sb.Append("\"lxrPauseSamplesMs\":[").Append(string.Join(",", allLxrMs.Select(v => v.ToString("F4", CultureInfo.InvariantCulture)))).Append("],");
            sb.Append("\"lxrPauseByType\":{");
            sb.Append(string.Join(",", lxrPauses.GroupBy(p => p.type).Select(g =>
                $"\"{g.Key}\":{{\"count\":{g.Count()},\"maxMs\":{g.Max(x => x.ms).ToString("F4", CultureInfo.InvariantCulture)},\"totalMs\":{g.Sum(x => x.ms).ToString("F4", CultureInfo.InvariantCulture)}}}")));
            sb.Append("},");
            sb.Append("\"suspendPauseSamplesMs\":[").Append(string.Join(",", suspendPauses.Select(v => v.ToString("F4", CultureInfo.InvariantCulture)))).Append("],");
            sb.Append("\"phases\":{");
            sb.Append(string.Join(",", phaseAgg.Select(kv =>
                $"\"{kv.Key}\":{{\"count\":{kv.Value.count},\"totalMs\":{kv.Value.total.ToString("F4", CultureInfo.InvariantCulture)},\"maxMs\":{kv.Value.max.ToString("F4", CultureInfo.InvariantCulture)},\"concurrent\":{kv.Value.concurrent}}}")));
            sb.Append("}");
            sb.Append("}");
            System.IO.File.WriteAllText(jsonOut, sb.ToString());
            Console.WriteLine($"wrote {jsonOut}");
        }
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
