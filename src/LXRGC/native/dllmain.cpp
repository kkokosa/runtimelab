// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
//
// dllmain.cpp - standalone GC loader entry points for LXRGC.
//
// CoreCLR discovers a standalone GC by LoadLibrary'ing the DLL named by
// DOTNET_GCName, calling GC_VersionInfo to check ABI compatibility, then
// GC_Initialize to obtain an IGCHeap + IGCHandleManager.
//
#include "LXRGC.h"

IGCToCLR* g_theGCToCLR = nullptr;
VersionInfo g_runtimeSupportedVersion = {};
bool g_oldMethodTableFlags = false;

static LXRGCHandleManager* g_lxrGCHandleManager = nullptr;

extern "C" __declspec(dllexport) void GC_VersionInfo(VersionInfo* info)
{
    g_runtimeSupportedVersion = *info;
    g_oldMethodTableFlags = g_runtimeSupportedVersion.MajorVersion < 2;

    info->MajorVersion = GC_INTERFACE_MAJOR_VERSION;
    info->MinorVersion = GC_INTERFACE_MINOR_VERSION;
    info->BuildVersion = 0;
    info->Name = "LXRGC";
}

extern "C" __declspec(dllexport) HRESULT GC_Initialize(
    IGCToCLR* clrToGC,
    IGCHeap** gcHeap,
    IGCHandleManager** gcHandleManager,
    GcDacVars* gcDacVars)
{
    if (gcHeap == nullptr || gcHandleManager == nullptr || gcDacVars == nullptr)
        return E_INVALIDARG;

    g_theGCToCLR = clrToGC;

    memset(gcDacVars, 0, sizeof(*gcDacVars));

    g_lxrGCHandleManager = new (nothrow) LXRGCHandleManager();
    if (g_lxrGCHandleManager == nullptr || !g_lxrGCHandleManager->Initialize())
        return E_OUTOFMEMORY;

    g_lxrGCHeap = LXRGCHeap::CreateAndInitialize();
    if (g_lxrGCHeap == nullptr)
        return E_OUTOFMEMORY;

    *gcHeap = g_lxrGCHeap;
    *gcHandleManager = g_lxrGCHandleManager;
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE hInstDll, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hInstDll);
    }
    return TRUE;
}
