// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
//
// LXRGCHandles.cpp - GC handle table.
//
// Reused essentially verbatim from ZeroGC: a never-shrinking segmented slot
// table. LXR *would* treat weak handles specially (clearing them when the RC
// engine proves the referent dead), but because the RC engine cannot be
// driven under the standalone ABI (see FEASIBILITY.md) the practical behaviour
// here is identical to ZeroGC's: whatever was stored last is returned.
//
#include "LXRGC.h"

LXRGCHandleStore::LXRGCHandleStore()
{
    InitializeCriticalSection(&m_lock);
}

LXRGCHandleStore::~LXRGCHandleStore()
{
    DeleteCriticalSection(&m_lock);
}

void LXRGCHandleStore::Uproot() { }

bool LXRGCHandleStore::ContainsHandle(OBJECTHANDLE handle)
{
    return handle != nullptr;
}

OBJECTHANDLE LXRGCHandleStore::AllocSlot(Object* value, HandleType type)
{
    EnterCriticalSection(&m_lock);

    Slot* slot;
    if (m_freeList != nullptr)
    {
        slot = m_freeList;
        m_freeList = slot->NextFree;
    }
    else
    {
        slot = new (nothrow) Slot();
    }

    LeaveCriticalSection(&m_lock);

    if (slot == nullptr)
        return nullptr;

    slot->Value = value;
    slot->Secondary = nullptr;
    slot->ExtraInfo = nullptr;
    slot->Type = type;
    slot->InUse = true;

    int64_t live = InterlockedIncrement64(&g_lxrCounters.LiveHandleCount);
    int64_t peak = g_lxrCounters.PeakHandleCount;
    while (live > peak)
    {
        int64_t prev = InterlockedCompareExchange64(&g_lxrCounters.PeakHandleCount, live, peak);
        if (prev == peak)
            break;
        peak = prev;
    }

    return (OBJECTHANDLE)slot;
}

void LXRGCHandleStore::FreeSlot(OBJECTHANDLE handle)
{
    if (handle == nullptr)
        return;

    Slot* slot = SlotFromHandle(handle);
    if (!slot->InUse)
        return;

    slot->InUse = false;
    slot->Value = nullptr;
    slot->Secondary = nullptr;

    EnterCriticalSection(&m_lock);
    slot->NextFree = m_freeList;
    m_freeList = slot;
    LeaveCriticalSection(&m_lock);

    InterlockedDecrement64(&g_lxrCounters.LiveHandleCount);
}

OBJECTHANDLE LXRGCHandleStore::CreateHandleOfType(Object* object, HandleType type)
{
    return AllocSlot(object, type);
}

OBJECTHANDLE LXRGCHandleStore::CreateHandleOfType(Object* object, HandleType type, int heapToAffinitizeTo)
{
    return AllocSlot(object, type);
}

OBJECTHANDLE LXRGCHandleStore::CreateHandleWithExtraInfo(Object* object, HandleType type, void* pExtraInfo)
{
    OBJECTHANDLE handle = AllocSlot(object, type);
    if (handle != nullptr)
        SlotFromHandle(handle)->ExtraInfo = pExtraInfo;
    return handle;
}

OBJECTHANDLE LXRGCHandleStore::CreateDependentHandle(Object* primary, Object* secondary)
{
    OBJECTHANDLE handle = AllocSlot(primary, HNDTYPE_DEPENDENT);
    if (handle != nullptr)
        SlotFromHandle(handle)->Secondary = secondary;
    return handle;
}

// ---------------------------------------------------------------------------
// LXRGCHandleManager
// ---------------------------------------------------------------------------
bool LXRGCHandleManager::Initialize()
{
    m_globalStore = new (nothrow) LXRGCHandleStore();
    return m_globalStore != nullptr;
}

void LXRGCHandleManager::Shutdown() { }

IGCHandleStore* LXRGCHandleManager::GetGlobalHandleStore()
{
    return m_globalStore;
}

IGCHandleStore* LXRGCHandleManager::CreateHandleStore()
{
    return new (nothrow) LXRGCHandleStore();
}

void LXRGCHandleManager::DestroyHandleStore(IGCHandleStore* store)
{
    delete store;
}

OBJECTHANDLE LXRGCHandleManager::CreateGlobalHandleOfType(Object* object, HandleType type)
{
    return m_globalStore->CreateHandleOfType(object, type);
}

OBJECTHANDLE LXRGCHandleManager::CreateDuplicateHandle(OBJECTHANDLE handle)
{
    LXRGCHandleStore::Slot* slot = LXRGCHandleStore::SlotFromHandle(handle);
    return m_globalStore->CreateHandleOfType(slot->Value, slot->Type);
}

void LXRGCHandleManager::DestroyHandleOfType(OBJECTHANDLE handle, HandleType type)
{
    m_globalStore->FreeSlot(handle);
}

void LXRGCHandleManager::DestroyHandleOfUnknownType(OBJECTHANDLE handle)
{
    m_globalStore->FreeSlot(handle);
}

void LXRGCHandleManager::SetExtraInfoForHandle(OBJECTHANDLE handle, HandleType type, void* pExtraInfo)
{
    LXRGCHandleStore::SlotFromHandle(handle)->ExtraInfo = pExtraInfo;
}

void* LXRGCHandleManager::GetExtraInfoFromHandle(OBJECTHANDLE handle)
{
    return LXRGCHandleStore::SlotFromHandle(handle)->ExtraInfo;
}

void LXRGCHandleManager::StoreObjectInHandle(OBJECTHANDLE handle, Object* object)
{
    LXRGCHandleStore::SlotFromHandle(handle)->Value = object;
}

bool LXRGCHandleManager::StoreObjectInHandleIfNull(OBJECTHANDLE handle, Object* object)
{
    LXRGCHandleStore::Slot* slot = LXRGCHandleStore::SlotFromHandle(handle);
    if (slot->Value == nullptr)
    {
        slot->Value = object;
        return true;
    }
    return false;
}

void LXRGCHandleManager::SetDependentHandleSecondary(OBJECTHANDLE handle, Object* object)
{
    LXRGCHandleStore::SlotFromHandle(handle)->Secondary = object;
}

Object* LXRGCHandleManager::GetDependentHandleSecondary(OBJECTHANDLE handle)
{
    return LXRGCHandleStore::SlotFromHandle(handle)->Secondary;
}

Object* LXRGCHandleManager::InterlockedCompareExchangeObjectInHandle(OBJECTHANDLE handle, Object* object, Object* comparandObject)
{
    LXRGCHandleStore::Slot* slot = LXRGCHandleStore::SlotFromHandle(handle);
    return (Object*)InterlockedCompareExchangePointer((PVOID*)&slot->Value, object, comparandObject);
}

HandleType LXRGCHandleManager::HandleFetchType(OBJECTHANDLE handle)
{
    return LXRGCHandleStore::SlotFromHandle(handle)->Type;
}

void LXRGCHandleManager::TraceRefCountedHandles(HANDLESCANPROC callback, uintptr_t param1, uintptr_t param2)
{
    // No ref-counted (COM interop) handle tracing in this experiment.
}
