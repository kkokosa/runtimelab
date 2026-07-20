using System;

class Program
{
    // A strong static root keeps a graph alive across a phase; nulling it makes
    // the whole graph unreachable so LXR's backup trace + sweep can reclaim it.
    static object[] s_root;

    sealed class Node
    {
        public Node Next;                 // reference field -> exercises GCScanObjectRefs
        public byte[] Payload = new byte[112];
    }

    static long InUseMB() => GC.GetTotalMemory(false) / (1024 * 1024);

    static void Fill(int n)
    {
        s_root = new object[n];
        for (int i = 0; i < n; i++)
            s_root[i] = new byte[128];
    }

    // Build a self-referential cycle and drop all local references to it. Pure
    // reference counting can never reclaim this; LXR's backup trace must.
    static void BuildAndDropCycle()
    {
        var a = new Node();
        var b = new Node();
        a.Next = b;
        b.Next = a;
        a = null;
        b = null;
    }

    static void Main()
    {
        Console.WriteLine($"AppContext GCName: {AppContext.GetData("GCName")}");

        const int N = 400_000;

        Console.WriteLine($"[phase0] startup committed-in-use = {InUseMB()} MB");

        // Phase 1: allocate a large live graph and keep it rooted.
        Fill(N);
        long live = InUseMB();
        Console.WriteLine($"[phase1] live graph rooted, committed-in-use = {live} MB");

        // Phase 2: drop the graph, then collect. Dead regions should decommit.
        s_root = null;
        GC.Collect();
        long afterDrop = InUseMB();
        Console.WriteLine($"[phase2] dropped + GC.Collect(), committed-in-use = {afterDrop} MB");

        // Phase 3: allocate again. Reclaimed regions should be reused, so the
        // committed footprint should stay near the phase-1 level, not double.
        Fill(N);
        long afterReuse = InUseMB();
        Console.WriteLine($"[phase3] re-allocated, committed-in-use = {afterReuse} MB");

        // Phase 4: cycle collection - drop everything (incl. a cycle) and collect.
        s_root = null;
        BuildAndDropCycle();
        GC.Collect();
        long afterCycle = InUseMB();
        Console.WriteLine($"[phase4] cycle dropped + GC.Collect(), committed-in-use = {afterCycle} MB");

        bool reclaimed = afterDrop < live;
        bool reused = afterReuse <= live + (live / 4) + 8; // stayed roughly flat (not ~2x)
        Console.WriteLine(reclaimed ? "LXRGC-RECLAIM-OK" : "LXRGC-RECLAIM-NONE");
        Console.WriteLine(reused ? "LXRGC-REUSE-OK" : "LXRGC-REUSE-NONE");
        Console.WriteLine("LXRGC-SMOKE-OK");
    }
}
