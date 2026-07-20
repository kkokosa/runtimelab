using System;
using System.Collections.Generic;

class Program
{
    static void Main()
    {
        Console.WriteLine($"GC backend: {System.Runtime.GCSettings.IsServerGC}, name via config below");
        Console.WriteLine($"AppContext GCName: {AppContext.GetData("GCName")}");

        long before = GC.GetTotalAllocatedBytes();
        var keep = new List<object>();
        for (int i = 0; i < 200_000; i++)
        {
            keep.Add(new byte[64]);
            if ((i & 0x3FFF) == 0)
                keep.Clear(); // drop references: a real GC could reclaim; LXR/RC would decrement here
        }
        long after = GC.GetTotalAllocatedBytes();

        Console.WriteLine($"Allocated ~{(after - before) / (1024 * 1024)} MB");
        Console.WriteLine($"CollectionCount(0)={GC.CollectionCount(0)}  CollectionCount(2)={GC.CollectionCount(2)}");
        Console.WriteLine($"TotalMemory={GC.GetTotalMemory(false) / (1024 * 1024)} MB");
        GC.Collect();
        Console.WriteLine($"After GC.Collect(): CollectionCount(2)={GC.CollectionCount(2)}");
        Console.WriteLine("LXRGC-SMOKE-OK");
    }
}
