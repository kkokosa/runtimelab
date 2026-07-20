// Microbenchmark validating the central perf claim behind gcobjscan.h:
//   an inlined *template* visitor (GCScanObjectRefs<TVisit>) generates the same
//   tight code as the hand-written macro (go_through_object), while a per-field
//   *function-pointer* callback (the literal #12809 API shape) is dramatically
//   slower because it defeats inlining.
//
// We reproduce the exact control flow of the CGCDesc "normal series" walk over a
// synthetic descriptor so the measurement isolates the inlining effect and does
// not depend on the runtime's MethodTable internals.
//
// Build (MSVC):  cl /O2 /EHsc bench_scan.cpp
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <chrono>

struct Series { uint32_t offset; uint32_t count; };

struct Desc {
    const Series* series;
    uint32_t      numSeries;
};

// ---- variant A/C: inlined template visitor (macro-equivalent codegen) --------
template <typename TVisit>
static inline void ScanTemplate(uint8_t* obj, const Desc* d, TVisit visit)
{
    for (uint32_t s = 0; s < d->numSeries; s++)
    {
        uint8_t** p = (uint8_t**)(obj + d->series[s].offset);
        uint8_t** end = p + d->series[s].count;
        while (p < end)
        {
            visit(p);
            p++;
        }
    }
}

// ---- variant B: per-field function-pointer callback (the #12809 API) --------
typedef void (*RefFn)(uint8_t** ref, void* ctx);
static void ScanFnPtr(uint8_t* obj, const Desc* d, RefFn fn, void* ctx)
{
    for (uint32_t s = 0; s < d->numSeries; s++)
    {
        uint8_t** p = (uint8_t**)(obj + d->series[s].offset);
        uint8_t** end = p + d->series[s].count;
        while (p < end)
        {
            fn(p, ctx);
            p++;
        }
    }
}

static volatile uint64_t g_sink = 0;

// The callback body is intentionally trivial (what a hot RC-decrement / mark
// visitor's inlined core looks like): read the ref and accumulate.
static void CallbackAccumulate(uint8_t** ref, void* ctx)
{
    uint64_t* acc = (uint64_t*)ctx;
    *acc += (uint64_t)(uintptr_t)(*ref);
}

int main()
{
    const uint32_t kFieldsPerObj = 16;
    const uint32_t kObjs = 200000;
    const int kReps = 200;

    // Build one series covering all fields; an object is kFieldsPerObj pointers.
    Series series = { 0, kFieldsPerObj };
    Desc desc = { &series, 1 };

    size_t objBytes = (size_t)kFieldsPerObj * sizeof(void*);
    uint8_t* heap = (uint8_t*)malloc((size_t)kObjs * objBytes);
    // Fill with non-null pseudo-pointers.
    uint8_t** slots = (uint8_t**)heap;
    for (size_t i = 0; i < (size_t)kObjs * kFieldsPerObj; i++)
        slots[i] = (uint8_t*)(uintptr_t)(i * 8 + 8);

    const uint64_t totalFields = (uint64_t)kObjs * kFieldsPerObj * kReps;

    // ---- Template (inlined lambda) ----
    {
        uint64_t acc = 0;
        auto t0 = std::chrono::high_resolution_clock::now();
        for (int r = 0; r < kReps; r++)
            for (uint32_t o = 0; o < kObjs; o++)
                ScanTemplate(heap + (size_t)o * objBytes, &desc,
                             [&acc](uint8_t** ref) { acc += (uint64_t)(uintptr_t)(*ref); });
        auto t1 = std::chrono::high_resolution_clock::now();
        g_sink += acc;
        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
        printf("template (inlined)   : %.3f ns/field  (acc=%llu)\n",
               ns / totalFields, (unsigned long long)acc);
    }

    // ---- Function pointer ----
    {
        uint64_t acc = 0;
        // Load the callback through a volatile so the compiler cannot see the
        // concrete callee and inline/devirtualize it — this mirrors the real
        // #12809 API where the GC registers an opaque function pointer the
        // runtime calls per field. That forces a true indirect call each field.
        volatile RefFn vfn = &CallbackAccumulate;
        RefFn fn = vfn;
        auto t0 = std::chrono::high_resolution_clock::now();
        for (int r = 0; r < kReps; r++)
            for (uint32_t o = 0; o < kObjs; o++)
                ScanFnPtr(heap + (size_t)o * objBytes, &desc, fn, &acc);
        auto t1 = std::chrono::high_resolution_clock::now();
        g_sink += acc;
        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
        printf("function pointer     : %.3f ns/field  (acc=%llu)\n",
               ns / totalFields, (unsigned long long)acc);
    }

    free(heap);
    printf("sink=%llu\n", (unsigned long long)g_sink);
    return 0;
}
