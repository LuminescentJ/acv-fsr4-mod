#include <windows.h>
#include <d3d12.h>
#include <strsafe.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

// FidelityFX API headers (vendored SDK, read-only). Only types/constants are
// used; the functions are resolved at runtime with GetProcAddress.
#include "ffx_api.h"
#include "ffx_api_types.h"
#include "dx12/ffx_api_dx12.h"
#include "ffx_upscale.h"

namespace {
INIT_ONCE g_d3d12InitOnce = INIT_ONCE_STATIC_INIT;
HMODULE g_systemD3d12 = nullptr;
SRWLOCK g_logLock = SRWLOCK_INIT;
SRWLOCK g_hookLock = SRWLOCK_INIT;
HANDLE g_logFile = INVALID_HANDLE_VALUE;
std::atomic<unsigned long> g_loggedResources{0};
std::array<std::atomic<unsigned long>, 5> g_loggedHookDescriptions{};
constexpr unsigned long kMaxLoggedResources = 12000;
constexpr unsigned long kMaxLoggedHookDescriptions = 512;
constexpr unsigned long kMaxLoggedNonTextureDescriptions = 32;
constexpr UINT64 kMinimumTextureWidth = 256;
constexpr UINT kMinimumTextureHeight = 256;

// ID3D12Device vtable slots (IUnknown 0-2, ID3D12Object 3-6, ID3D12Device 7+).
// The earlier build used 29/31/32, which are CreatePlacedResource,
// CreateSharedHandle and OpenSharedHandle. Correct slots:
constexpr size_t kSlotQueryInterface = 0;
constexpr size_t kSlotCreateGraphicsPipelineState = 10;
constexpr size_t kSlotCreateDescriptorHeap = 14;
constexpr size_t kSlotCreateRootSignature = 16;
constexpr size_t kSlotCreateConstantBufferView = 17;
constexpr size_t kSlotCreateShaderResourceView = 18;
constexpr size_t kSlotCreateUnorderedAccessView = 19;
constexpr size_t kSlotCreateRenderTargetView = 20;
constexpr size_t kSlotCreateDepthStencilView = 21;
constexpr size_t kSlotCopyDescriptors = 23;
constexpr size_t kSlotCopyDescriptorsSimple = 24;
constexpr size_t kSlotCreateCommittedResource = 27;
constexpr size_t kSlotCreatePlacedResource = 29;
constexpr size_t kSlotCreateReservedResource = 30;
constexpr size_t kSlotCreatePipelineLibrary = 44;   // ID3D12Device1
constexpr size_t kSlotCreatePipelineState = 47;     // ID3D12Device2
constexpr size_t kDeviceOriginalSlots = kSlotCreatePipelineState + 1;

constexpr const char* kSystemExports[] = {
    nullptr, // ordinal 99 is intentionally unnamed on Windows 11.
    "SetAppCompatStringPointer",
    "D3D12CreateDevice",
    "D3D12GetDebugInterface",
    "D3D12CoreCreateLayeredDevice",
    "D3D12CoreGetLayeredDeviceSize",
    "D3D12CoreRegisterLayers",
    "D3D12CreateRootSignatureDeserializer",
    "D3D12CreateVersionedRootSignatureDeserializer",
    "D3D12DeviceRemovedExtendedData",
    "D3D12EnableExperimentalFeatures",
    "D3D12GetInterface",
    "D3D12PIXEventsReplaceBlock",
    "D3D12PIXGetThreadInfo",
    "D3D12PIXNotifyWakeFromFenceSignal",
    "D3D12PIXReportCounter",
    "D3D12SerializeRootSignature",
    "D3D12SerializeVersionedRootSignature",
    "GetBehaviorValue",
};

using D3D12CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using QueryInterfaceFn = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, REFIID, void**);
using CreateCommittedResourceFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
    const D3D12_CLEAR_VALUE*, REFIID, void**);
using CreatePlacedResourceFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
    D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using CreateReservedResourceFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device*, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
    const D3D12_CLEAR_VALUE*, REFIID, void**);
using CreateDescriptorHeapFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
using CreateRootSignatureFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
using CreateCbvFn = void(STDMETHODCALLTYPE*)(
    ID3D12Device*, const D3D12_CONSTANT_BUFFER_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using CreateSrvFn = void(STDMETHODCALLTYPE*)(
    ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using CreateUavFn = void(STDMETHODCALLTYPE*)(
    ID3D12Device*, ID3D12Resource*, ID3D12Resource*, const D3D12_UNORDERED_ACCESS_VIEW_DESC*,
    D3D12_CPU_DESCRIPTOR_HANDLE);
using CreateRtvFn = void(STDMETHODCALLTYPE*)(
    ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using CreateDsvFn = void(STDMETHODCALLTYPE*)(
    ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using CopyDescriptorsFn = void(STDMETHODCALLTYPE*)(
    ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*,
    UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
using CopyDescriptorsSimpleFn = void(STDMETHODCALLTYPE*)(
    ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE,
    D3D12_DESCRIPTOR_HEAP_TYPE);

struct DeviceOriginals {
    void** originalVtable = nullptr;
    size_t vtableLength = 0;
    void* slots[kDeviceOriginalSlots]{};  // Original function for every slot we may hook.
};

HRESULT STDMETHODCALLTYPE HookedQueryInterface(IUnknown* self, REFIID iid, void** objectOut);
bool patchDevice(ID3D12Device* device, size_t vtableLength);

std::unordered_map<void**, DeviceOriginals> g_deviceOriginals;

BOOL CALLBACK loadSystemD3d12(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t systemPath[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(systemPath, ARRAYSIZE(systemPath));
    if (length == 0 || length + 12 >= ARRAYSIZE(systemPath)) return TRUE;
    if (FAILED(StringCchCatW(systemPath, ARRAYSIZE(systemPath), L"\\d3d12.dll"))) return TRUE;
    g_systemD3d12 = LoadLibraryW(systemPath);
    return TRUE;
}

HMODULE systemD3d12() {
    InitOnceExecuteOnce(&g_d3d12InitOnce, loadSystemD3d12, nullptr, nullptr);
    return g_systemD3d12;
}

// Release build (ACV_FSR4_RELEASE=1, the installable mod): only the FSR 4 path. Investigation features stay compiled
// but inert (no hotkeys / hooks / logs for them), FFX debug checking is off, and it keeps a small log of its own.
#ifndef ACV_FSR4_RELEASE
#define ACV_FSR4_RELEASE 0
#endif
constexpr bool kRelease = ACV_FSR4_RELEASE != 0;
constexpr unsigned kReleaseMaxLogLines = 2000;
std::atomic<unsigned> g_logLines{0};

void openLogIfNeeded() {
    if (g_logFile != INVALID_HANDLE_VALUE) return;
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", path, ARRAYSIZE(path));
    if (length == 0 || length >= ARRAYSIZE(path)) return;
    if (FAILED(StringCchCatW(path, ARRAYSIZE(path), kRelease ? L"\\ACVFSR4" : L"\\ACVFSR31"))) return;
    CreateDirectoryW(path, nullptr);
    if (FAILED(StringCchCatW(path, ARRAYSIZE(path), kRelease ? L"\\acv_fsr4.log" : L"\\d3d12_probe.log"))) return;
    // Release: a fresh log per game session. Probe: appended (the test script clears it).
    g_logFile = CreateFileW(path, kRelease ? GENERIC_WRITE : FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, kRelease ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void logLine(const char* format, ...) {
    if (kRelease) {
        // Only the mod's own lines, and a bounded number of them.
        const bool ours = strncmp(format, "FSR_", 4) == 0 || strncmp(format, "TAA_", 4) == 0 || strncmp(format, "ACV_", 4) == 0;
        if (!ours || g_logLines.fetch_add(1) >= kReleaseMaxLogLines) return;
    }
    char line[1024]{};
    va_list args;
    va_start(args, format);
    const int count = vsnprintf(line, sizeof(line) - 3, format, args);
    va_end(args);
    if (count < 0) return;
    const size_t length = static_cast<size_t>(count) < sizeof(line) - 3
        ? static_cast<size_t>(count) : sizeof(line) - 3;
    line[length] = '\r';
    line[length + 1] = '\n';

    AcquireSRWLockExclusive(&g_logLock);
    openLogIfNeeded();
    if (g_logFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_logFile, line, static_cast<DWORD>(length + 2), &written, nullptr);
    }
    ReleaseSRWLockExclusive(&g_logLock);
}

void* lookupExport(UINT ordinalIndex) {
    HMODULE module = systemD3d12();
    if (!module || ordinalIndex >= ARRAYSIZE(kSystemExports)) return nullptr;
    if (ordinalIndex == 0) return reinterpret_cast<void*>(GetProcAddress(module, MAKEINTRESOURCEA(99)));
    return reinterpret_cast<void*>(GetProcAddress(module, kSystemExports[ordinalIndex]));
}

bool isLoggableTexture(const D3D12_RESOURCE_DESC* desc) {
    return desc && desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        desc->Width >= kMinimumTextureWidth && desc->Height >= kMinimumTextureHeight;
}

void logHookDescription(const char* kind, ID3D12Device* self,
    const D3D12_RESOURCE_DESC* desc) {
    if (kRelease) return;
    const UINT dimension = desc ? static_cast<UINT>(desc->Dimension) : 0;
    const size_t bucket = dimension < g_loggedHookDescriptions.size() ? dimension : 4;
    const unsigned long number = g_loggedHookDescriptions[bucket].fetch_add(1, std::memory_order_relaxed);
    const unsigned long limit = dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D
        ? kMaxLoggedHookDescriptions : kMaxLoggedNonTextureDescriptions;
    if (number >= limit) return;
    if (!desc) {
        logLine("HOOK_CALL #%lu kind=%s self=%p desc=null", number + 1, kind, self);
        return;
    }
    logLine("HOOK_CALL #%lu kind=%s self=%p dimension=%u width=%llu height=%u depthOrArray=%u mips=%u format=%u samples=%u flags=0x%08X",
        number + 1, kind, self, static_cast<unsigned>(desc->Dimension),
        static_cast<unsigned long long>(desc->Width), desc->Height,
        desc->DepthOrArraySize, desc->MipLevels, static_cast<unsigned>(desc->Format),
        desc->SampleDesc.Count, static_cast<unsigned>(desc->Flags));
}

void logResource(const char* kind, ID3D12Resource* resource,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES initialState,
    UINT heapType, D3D12_HEAP_FLAGS heapFlags, UINT64 offset = 0) {
    if (kRelease) return;
    if (!resource || !isLoggableTexture(desc)) return;
    const unsigned long number = g_loggedResources.fetch_add(1, std::memory_order_relaxed);
    if (number >= kMaxLoggedResources) return;

    logLine("RESOURCE #%lu kind=%s ptr=%p width=%llu height=%u depthOrArray=%u mips=%u format=%u samples=%u flags=0x%08X state=0x%08X heapType=%u heapFlags=0x%08X offset=%llu",
        number + 1, kind, resource, static_cast<unsigned long long>(desc->Width),
        desc->Height, desc->DepthOrArraySize, desc->MipLevels,
        static_cast<unsigned>(desc->Format), desc->SampleDesc.Count,
        static_cast<unsigned>(desc->Flags), static_cast<unsigned>(initialState),
        heapType, static_cast<unsigned>(heapFlags), static_cast<unsigned long long>(offset));
}

void* getDeviceOriginal(ID3D12Device* self, size_t slot) {
    if (!self) return nullptr;
    void** table = *reinterpret_cast<void***>(self);
    AcquireSRWLockShared(&g_hookLock);
    auto it = g_deviceOriginals.find(table);
    void* original = nullptr;
    if (it != g_deviceOriginals.end() && slot < kDeviceOriginalSlots) original = it->second.slots[slot];
    ReleaseSRWLockShared(&g_hookLock);
    return original;
}

size_t deviceVtableLength(REFIID iid) {
    if (IsEqualIID(iid, __uuidof(ID3D12Device))) return 47;
    if (IsEqualIID(iid, __uuidof(ID3D12Device1))) return 50;
    if (IsEqualIID(iid, __uuidof(ID3D12Device2))) return 51;
    if (IsEqualIID(iid, __uuidof(ID3D12Device3))) return 54;
    if (IsEqualIID(iid, __uuidof(ID3D12Device4))) return 61;
    if (IsEqualIID(iid, __uuidof(ID3D12Device5))) return 69;
    if (IsEqualIID(iid, __uuidof(ID3D12Device6))) return 70;
    if (IsEqualIID(iid, __uuidof(ID3D12Device7))) return 72;
    if (IsEqualIID(iid, __uuidof(ID3D12Device8))) return 78;
    if (IsEqualIID(iid, __uuidof(ID3D12Device9))) return 81;
    if (IsEqualIID(iid, __uuidof(ID3D12Device10))) return 84;
    if (IsEqualIID(iid, __uuidof(ID3D12Device11))) return 85;
    if (IsEqualIID(iid, __uuidof(ID3D12Device12))) return 87;
    if (IsEqualIID(iid, __uuidof(ID3D12Device13))) return 88;
    if (IsEqualIID(iid, __uuidof(ID3D12Device14))) return 89;
    return 0;
}

HRESULT STDMETHODCALLTYPE HookedQueryInterface(IUnknown* self, REFIID iid, void** objectOut) {
    if (!self) return E_POINTER;
    void** table = *reinterpret_cast<void***>(self);
    AcquireSRWLockShared(&g_hookLock);
    auto it = g_deviceOriginals.find(table);
    auto original = it == g_deviceOriginals.end()
        ? nullptr : reinterpret_cast<QueryInterfaceFn>(it->second.slots[kSlotQueryInterface]);
    ReleaseSRWLockShared(&g_hookLock);
    if (!original) return E_NOINTERFACE;

    const HRESULT hr = original(self, iid, objectOut);
    const size_t requestedLength = deviceVtableLength(iid);
    if (SUCCEEDED(hr) && objectOut && *objectOut && requestedLength != 0) {
        auto* deviceInterface = reinterpret_cast<ID3D12Device*>(*objectOut);
        patchDevice(deviceInterface, requestedLength);
    }
    return hr;
}

// CPU-mappable buffers by GPU virtual address range, so a CBV's address can be read back on the CPU (F8 constants capture).
// Raw pointers, no AddRef: a range is only looked up while the game is binding a CBV inside it, so the buffer is alive.
struct UploadBuffer { UINT64 va; UINT64 size; ID3D12Resource* resource; };
SRWLOCK g_uploadLock = SRWLOCK_INIT;
std::vector<UploadBuffer> g_uploadBuffers;   // Sorted by va.

bool isCpuVisibleHeap(const D3D12_HEAP_PROPERTIES& p) {
    return p.Type == D3D12_HEAP_TYPE_UPLOAD ||
        (p.Type == D3D12_HEAP_TYPE_CUSTOM && p.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE &&
         p.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_UNKNOWN);
}

void trackUploadBuffer(void* object, const D3D12_RESOURCE_DESC* desc) {
    if (!object || !desc || desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return;
    ID3D12Resource* resource = nullptr;
    if (FAILED(static_cast<IUnknown*>(object)->QueryInterface(__uuidof(ID3D12Resource),
            reinterpret_cast<void**>(&resource))) || !resource) return;
    const UploadBuffer rec{resource->GetGPUVirtualAddress(), desc->Width, resource};
    resource->Release();
    if (!rec.va) return;
    AcquireSRWLockExclusive(&g_uploadLock);
    try {
        // A new buffer over an old range means the old one is gone (or aliased): drop overlapping records.
        for (size_t i = 0; i < g_uploadBuffers.size();) {
            const UploadBuffer& b = g_uploadBuffers[i];
            if (b.va < rec.va + rec.size && rec.va < b.va + b.size) g_uploadBuffers.erase(g_uploadBuffers.begin() + static_cast<ptrdiff_t>(i));
            else ++i;
        }
        size_t pos = 0;
        while (pos < g_uploadBuffers.size() && g_uploadBuffers[pos].va < rec.va) ++pos;
        g_uploadBuffers.insert(g_uploadBuffers.begin() + static_cast<ptrdiff_t>(pos), rec);
    } catch (...) {}
    ReleaseSRWLockExclusive(&g_uploadLock);
}

// Copies up to maxBytes of the constant buffer at va (CPU read of the mapped upload buffer). Returns bytes copied.
size_t readUploadMemory(UINT64 va, size_t size, void* out, size_t maxBytes) {
    ID3D12Resource* resource = nullptr;
    UINT64 offset = 0;
    size_t bytes = 0;
    AcquireSRWLockShared(&g_uploadLock);
    size_t lo = 0, hi = g_uploadBuffers.size();
    while (lo < hi) { const size_t mid = (lo + hi) / 2; if (g_uploadBuffers[mid].va <= va) lo = mid + 1; else hi = mid; }
    if (lo > 0) {
        const UploadBuffer& b = g_uploadBuffers[lo - 1];
        if (va < b.va + b.size) {
            resource = b.resource;
            offset = va - b.va;
            const UINT64 avail = b.size - offset;
            bytes = static_cast<size_t>(size < avail ? size : avail);
            if (bytes > maxBytes) bytes = maxBytes;
        }
    }
    if (resource && bytes) {
        void* mapped = nullptr;
        const D3D12_RANGE readRange{static_cast<SIZE_T>(offset), static_cast<SIZE_T>(offset + bytes)};
        if (SUCCEEDED(resource->Map(0, &readRange, &mapped)) && mapped) {
            std::memcpy(out, static_cast<const uint8_t*>(mapped) + offset, bytes);
            const D3D12_RANGE noWrite{0, 0};
            resource->Unmap(0, &noWrite);
        } else {
            bytes = 0;
        }
    } else {
        bytes = 0;
    }
    ReleaseSRWLockShared(&g_uploadLock);
    return bytes;
}

HRESULT STDMETHODCALLTYPE HookedCreateCommittedResource(ID3D12Device* self,
    const D3D12_HEAP_PROPERTIES* heapProperties, D3D12_HEAP_FLAGS heapFlags,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES initialState,
    const D3D12_CLEAR_VALUE* clearValue, REFIID riid, void** resourceOut) {
    logHookDescription("CreateCommittedResource", self, desc);
    auto original = reinterpret_cast<CreateCommittedResourceFn>(getDeviceOriginal(self, kSlotCreateCommittedResource));
    if (!original) return E_NOINTERFACE;
    const HRESULT hr = original(self, heapProperties, heapFlags, desc, initialState,
        clearValue, riid, resourceOut);
    if (SUCCEEDED(hr) && resourceOut && *resourceOut && isLoggableTexture(desc)) {
        logResource("committed", static_cast<ID3D12Resource*>(*resourceOut), desc,
            initialState, heapProperties ? static_cast<UINT>(heapProperties->Type) : static_cast<UINT>(-1),
            heapFlags);
    }
    if (SUCCEEDED(hr) && resourceOut && *resourceOut && heapProperties && isCpuVisibleHeap(*heapProperties)) {
        trackUploadBuffer(*resourceOut, desc);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedCreatePlacedResource(ID3D12Device* self,
    ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC* desc,
    D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE* clearValue,
    REFIID riid, void** resourceOut) {
    logHookDescription("CreatePlacedResource", self, desc);
    auto original = reinterpret_cast<CreatePlacedResourceFn>(getDeviceOriginal(self, kSlotCreatePlacedResource));
    if (!original) return E_NOINTERFACE;
    const HRESULT hr = original(self, heap, offset, desc, initialState, clearValue, riid, resourceOut);
    if (SUCCEEDED(hr) && resourceOut && *resourceOut && isLoggableTexture(desc)) {
        logResource("placed", static_cast<ID3D12Resource*>(*resourceOut), desc,
            initialState, static_cast<UINT>(-1), D3D12_HEAP_FLAG_NONE, offset);
    }
    if (SUCCEEDED(hr) && resourceOut && *resourceOut && heap && desc &&
        desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        const D3D12_HEAP_DESC heapDesc = heap->GetDesc();
        if (isCpuVisibleHeap(heapDesc.Properties)) trackUploadBuffer(*resourceOut, desc);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedCreateReservedResource(ID3D12Device* self,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES initialState,
    const D3D12_CLEAR_VALUE* clearValue, REFIID riid, void** resourceOut) {
    logHookDescription("CreateReservedResource", self, desc);
    auto original = reinterpret_cast<CreateReservedResourceFn>(getDeviceOriginal(self, kSlotCreateReservedResource));
    if (!original) return E_NOINTERFACE;
    const HRESULT hr = original(self, desc, initialState, clearValue, riid, resourceOut);
    if (SUCCEEDED(hr) && resourceOut && *resourceOut && isLoggableTexture(desc)) {
        logResource("reserved", static_cast<ID3D12Resource*>(*resourceOut), desc,
            initialState, static_cast<UINT>(-1), D3D12_HEAP_FLAG_NONE);
    }
    return hr;
}

// ============================================================================
// Descriptor tracking. Every non-sampler descriptor heap gets a shadow array
// that records, per descriptor, which resource a view points at (with the
// resource's size/format captured when the view was written, so the pointer
// is never dereferenced later). View creation and descriptor copies keep the
// shadow arrays current; the frame trace resolves descriptor-table binds and
// render targets through them.
// ============================================================================
enum DescKind : uint8_t { kDescNone = 0, kDescCbv, kDescSrv, kDescUav, kDescRtv, kDescDsv };
constexpr const char* kDescKindNames[] = {"-", "CBV", "SRV", "UAV", "RTV", "DSV"};
constexpr UINT kMaxTrackedDescriptorsPerHeap = 4u * 1024u * 1024u;

struct DescEntry {
    void* resource;
    uint32_t width;
    uint32_t height;
    uint16_t resFormat;
    uint16_t viewFormat;
    uint8_t kind;
    uint8_t viewDim;
    uint8_t resDim;
    uint8_t arraySize;
};

struct HeapRecord {
    SIZE_T cpuStart = 0;
    UINT64 gpuStart = 0;       // 0 for non-shader-visible heaps.
    UINT count = 0;            // Tracked descriptors (may be capped).
    UINT increment = 0;
    UINT type = 0;
    DescEntry* entries = nullptr;
};

SRWLOCK g_heapLock = SRWLOCK_INIT;
std::vector<HeapRecord> g_heaps;  // Sorted by cpuStart.
std::atomic<unsigned long> g_loggedHeaps{0};

// Caller holds g_heapLock (shared or exclusive).
HeapRecord* findHeapByCpuLocked(SIZE_T cpu) {
    size_t lo = 0, hi = g_heaps.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (g_heaps[mid].cpuStart <= cpu) lo = mid + 1; else hi = mid;
    }
    if (lo == 0) return nullptr;
    HeapRecord& h = g_heaps[lo - 1];
    if (cpu >= h.cpuStart + static_cast<SIZE_T>(h.count) * h.increment) return nullptr;
    return &h;
}

DescEntry* findEntryByCpuLocked(SIZE_T cpu) {
    HeapRecord* h = findHeapByCpuLocked(cpu);
    if (!h || (cpu - h->cpuStart) % h->increment != 0) return nullptr;
    return &h->entries[(cpu - h->cpuStart) / h->increment];
}

HeapRecord* findHeapByGpuLocked(UINT64 gpu, UINT* indexOut) {
    for (HeapRecord& h : g_heaps) {
        if (h.gpuStart && gpu >= h.gpuStart && gpu < h.gpuStart + static_cast<UINT64>(h.count) * h.increment) {
            *indexOut = static_cast<UINT>((gpu - h.gpuStart) / h.increment);
            return &h;
        }
    }
    return nullptr;
}

void trackHeap(ID3D12Device* device, ID3D12DescriptorHeap* heap) {
    const D3D12_DESCRIPTOR_HEAP_DESC desc = heap->GetDesc();
    if (desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER || desc.NumDescriptors == 0) return;
    HeapRecord rec;
    rec.cpuStart = heap->GetCPUDescriptorHandleForHeapStart().ptr;
    rec.gpuStart = (desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
        ? heap->GetGPUDescriptorHandleForHeapStart().ptr : 0;
    rec.count = desc.NumDescriptors < kMaxTrackedDescriptorsPerHeap
        ? desc.NumDescriptors : kMaxTrackedDescriptorsPerHeap;
    rec.increment = device->GetDescriptorHandleIncrementSize(desc.Type);
    rec.type = static_cast<UINT>(desc.Type);
    if (rec.increment == 0) return;
    rec.entries = static_cast<DescEntry*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        static_cast<SIZE_T>(rec.count) * sizeof(DescEntry)));
    if (!rec.entries) return;

    const SIZE_T end = rec.cpuStart + static_cast<SIZE_T>(rec.count) * rec.increment;
    AcquireSRWLockExclusive(&g_heapLock);
    // A released heap's address range can be reused; drop overlapping stale records.
    for (size_t i = 0; i < g_heaps.size();) {
        const HeapRecord& h = g_heaps[i];
        const SIZE_T hEnd = h.cpuStart + static_cast<SIZE_T>(h.count) * h.increment;
        if (h.cpuStart < end && rec.cpuStart < hEnd) {
            HeapFree(GetProcessHeap(), 0, h.entries);
            g_heaps.erase(g_heaps.begin() + static_cast<ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    size_t pos = 0;
    while (pos < g_heaps.size() && g_heaps[pos].cpuStart < rec.cpuStart) ++pos;
    g_heaps.insert(g_heaps.begin() + static_cast<ptrdiff_t>(pos), rec);
    const size_t total = g_heaps.size();
    ReleaseSRWLockExclusive(&g_heapLock);

    if (g_loggedHeaps.fetch_add(1) < 256 || rec.gpuStart) {
        logLine("DESC_HEAP heap=%p type=%u num=%u tracked=%u visible=%d cpu=0x%llX gpu=0x%llX inc=%u heaps=%llu",
            heap, rec.type, desc.NumDescriptors, rec.count, rec.gpuStart ? 1 : 0,
            static_cast<unsigned long long>(rec.cpuStart), static_cast<unsigned long long>(rec.gpuStart),
            rec.increment, static_cast<unsigned long long>(total));
    }
}

void recordView(SIZE_T cpu, DescKind kind, ID3D12Resource* resource, DXGI_FORMAT viewFormat, UINT viewDim) {
    DescEntry e{};
    e.kind = kind;
    e.viewFormat = static_cast<uint16_t>(viewFormat);
    e.viewDim = static_cast<uint8_t>(viewDim);
    if (resource) {
        const D3D12_RESOURCE_DESC d = resource->GetDesc();
        e.resource = resource;
        e.width = d.Width > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(d.Width);
        e.height = d.Height;
        e.resFormat = static_cast<uint16_t>(d.Format);
        e.resDim = static_cast<uint8_t>(d.Dimension);
        e.arraySize = static_cast<uint8_t>(d.DepthOrArraySize > 255 ? 255 : d.DepthOrArraySize);
        if (viewFormat == DXGI_FORMAT_UNKNOWN) e.viewFormat = e.resFormat;
    }
    AcquireSRWLockShared(&g_heapLock);
    if (DescEntry* slot = findEntryByCpuLocked(cpu)) *slot = e;
    ReleaseSRWLockShared(&g_heapLock);
}

// Caller holds g_heapLock shared. Copies shadow entries for n descriptors.
void copyEntriesLocked(SIZE_T dst, SIZE_T src, UINT n) {
    HeapRecord* dh = findHeapByCpuLocked(dst);
    if (!dh) return;
    const UINT di = static_cast<UINT>((dst - dh->cpuStart) / dh->increment);
    const UINT dn = n < dh->count - di ? n : dh->count - di;
    HeapRecord* sh = findHeapByCpuLocked(src);
    if (!sh) { std::memset(&dh->entries[di], 0, static_cast<size_t>(dn) * sizeof(DescEntry)); return; }
    const UINT si = static_cast<UINT>((src - sh->cpuStart) / sh->increment);
    const UINT sn = dn < sh->count - si ? dn : sh->count - si;
    std::memmove(&dh->entries[di], &sh->entries[si], static_cast<size_t>(sn) * sizeof(DescEntry));
}

// Root signatures: descriptor-table layout per root parameter.
struct RsRange { uint8_t kind; UINT count; UINT offset; };
struct RsParam { bool table = false; UINT srvCount = 0; std::vector<RsRange> ranges; };
SRWLOCK g_rsLock = SRWLOCK_INIT;
std::unordered_map<void*, std::vector<RsParam>> g_rootSigs;
std::unordered_map<void*, std::string> g_rootSigBlobs;   // Serialized blob, for the F8 dump.
std::atomic<unsigned long> g_loggedRootSigs{0};
using CreateVersionedDeserializerFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);

void trackRootSignature(void* rootSig, const void* blob, SIZE_T size) {
    auto create = reinterpret_cast<CreateVersionedDeserializerFn>(lookupExport(8));
    if (!create || !blob) return;
    ID3D12VersionedRootSignatureDeserializer* deserializer = nullptr;
    if (FAILED(create(blob, size, __uuidof(ID3D12VersionedRootSignatureDeserializer),
            reinterpret_cast<void**>(&deserializer))) || !deserializer) return;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* vd = nullptr;
    std::vector<RsParam> params;
    unsigned tables = 0, unbounded = 0;
    if (SUCCEEDED(deserializer->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &vd)) && vd) {
        const D3D12_ROOT_SIGNATURE_DESC1& rs = vd->Desc_1_1;
        params.resize(rs.NumParameters);
        for (UINT p = 0; p < rs.NumParameters; ++p) {
            const D3D12_ROOT_PARAMETER1& rp = rs.pParameters[p];
            if (rp.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) continue;
            params[p].table = true;
            ++tables;
            UINT running = 0;
            for (UINT r = 0; r < rp.DescriptorTable.NumDescriptorRanges; ++r) {
                const D3D12_DESCRIPTOR_RANGE1& range = rp.DescriptorTable.pDescriptorRanges[r];
                const UINT offset = range.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                    ? running : range.OffsetInDescriptorsFromTableStart;
                uint8_t kind = kDescNone;
                switch (range.RangeType) {
                case D3D12_DESCRIPTOR_RANGE_TYPE_SRV: kind = kDescSrv; break;
                case D3D12_DESCRIPTOR_RANGE_TYPE_UAV: kind = kDescUav; break;
                case D3D12_DESCRIPTOR_RANGE_TYPE_CBV: kind = kDescCbv; break;
                default: break;  // Samplers live in a separate heap; ignored.
                }
                if (range.NumDescriptors == UINT_MAX) ++unbounded;
                else running = offset + range.NumDescriptors;
                if (kind == kDescSrv) params[p].srvCount += range.NumDescriptors == UINT_MAX ? 64u : range.NumDescriptors;
                if (kind != kDescNone) params[p].ranges.push_back({kind, range.NumDescriptors, offset});
            }
        }
    }
    deserializer->Release();
    if (g_loggedRootSigs.fetch_add(1) < 1024) {
        logLine("ROOTSIG rs=%p params=%llu tables=%u unbounded_ranges=%u",
            rootSig, static_cast<unsigned long long>(params.size()), tables, unbounded);
    }
    AcquireSRWLockExclusive(&g_rsLock);
    try {
        g_rootSigs[rootSig] = std::move(params);
        g_rootSigBlobs[rootSig].assign(static_cast<const char*>(blob), size);
    } catch (...) {}
    ReleaseSRWLockExclusive(&g_rsLock);
}

// Human-readable root signature (for the F8 dump): parameter types, registers, spaces, ranges.
std::string describeRootSignature(const std::string& blob) {
    std::string out;
    auto create = reinterpret_cast<CreateVersionedDeserializerFn>(lookupExport(8));
    ID3D12VersionedRootSignatureDeserializer* deserializer = nullptr;
    if (!create || blob.empty() || FAILED(create(blob.data(), blob.size(),
            __uuidof(ID3D12VersionedRootSignatureDeserializer), reinterpret_cast<void**>(&deserializer))) || !deserializer) {
        return "rootsig: could not deserialize\r\n";
    }
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* vd = nullptr;
    char line[256];
    if (SUCCEEDED(deserializer->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &vd)) && vd) {
        const D3D12_ROOT_SIGNATURE_DESC1& rs = vd->Desc_1_1;
        static const char* kTypes[] = {"TABLE", "CONSTANTS", "CBV", "SRV", "UAV"};
        static const char* kRanges[] = {"SRV", "UAV", "CBV", "SAMPLER"};
        snprintf(line, sizeof(line), "params=%u static_samplers=%u flags=0x%X\r\n",
            rs.NumParameters, rs.NumStaticSamplers, static_cast<unsigned>(rs.Flags));
        out += line;
        for (UINT p = 0; p < rs.NumParameters; ++p) {
            const D3D12_ROOT_PARAMETER1& rp = rs.pParameters[p];
            const UINT type = static_cast<UINT>(rp.ParameterType);
            snprintf(line, sizeof(line), "p%u %s visibility=%u", p, type < 5 ? kTypes[type] : "?",
                static_cast<unsigned>(rp.ShaderVisibility));
            out += line;
            if (rp.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
                snprintf(line, sizeof(line), " b%u space%u num32=%u", rp.Constants.ShaderRegister,
                    rp.Constants.RegisterSpace, rp.Constants.Num32BitValues);
                out += line;
            } else if (rp.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
                snprintf(line, sizeof(line), " reg%u space%u", rp.Descriptor.ShaderRegister, rp.Descriptor.RegisterSpace);
                out += line;
            }
            out += "\r\n";
            if (rp.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) continue;
            for (UINT r = 0; r < rp.DescriptorTable.NumDescriptorRanges; ++r) {
                const D3D12_DESCRIPTOR_RANGE1& range = rp.DescriptorTable.pDescriptorRanges[r];
                const UINT rt = static_cast<UINT>(range.RangeType);
                snprintf(line, sizeof(line), "    range%u %s base=%u count=%d space=%u offset=%d\r\n", r,
                    rt < 4 ? kRanges[rt] : "?", range.BaseShaderRegister,
                    range.NumDescriptors == UINT_MAX ? -1 : static_cast<int>(range.NumDescriptors),
                    range.RegisterSpace, range.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                        ? -1 : static_cast<int>(range.OffsetInDescriptorsFromTableStart));
                out += line;
            }
        }
    }
    deserializer->Release();
    return out;
}

// Pixel-shader bytecode per pipeline state, deduplicated by content hash.
SRWLOCK g_shaderLock = SRWLOCK_INIT;
std::unordered_map<uint64_t, std::string> g_pixelShaders;
std::unordered_map<void*, uint64_t> g_psoPixelShader;
std::unordered_map<void*, void*> g_psoRootSig;
std::atomic<unsigned long> g_psoCreated{0};

uint64_t fnv1a(const void* data, size_t size) {
    uint64_t h = 1469598103934665603ull;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

void recordPso(const char* path, void* pso, void* rootSig, const D3D12_SHADER_BYTECODE& ps) {
    const unsigned long n = g_psoCreated.fetch_add(1) + 1;
    if (n == 1 || n % 1000 == 0) logLine("PSO_CREATED count=%lu path=%s", n, path);
    if (!pso || !ps.pShaderBytecode || ps.BytecodeLength == 0 || ps.BytecodeLength > (16u << 20)) return;
    const uint64_t hash = fnv1a(ps.pShaderBytecode, ps.BytecodeLength);
    AcquireSRWLockExclusive(&g_shaderLock);
    try {
        auto it = g_pixelShaders.find(hash);
        if (it == g_pixelShaders.end())
            g_pixelShaders.emplace(hash, std::string(static_cast<const char*>(ps.pShaderBytecode), ps.BytecodeLength));
        g_psoPixelShader[pso] = hash;
        g_psoRootSig[pso] = rootSig;
    } catch (...) {}
    ReleaseSRWLockExclusive(&g_shaderLock);
}

// Finds the PS and root signature in a pipeline state stream (CreatePipelineState / LoadPipeline).
// Each subobject is { type (pointer-aligned), payload } with the whole entry padded to pointer size.
bool parsePipelineStream(const D3D12_PIPELINE_STATE_STREAM_DESC* desc, D3D12_SHADER_BYTECODE* ps, void** rootSig) {
    if (!desc || !desc->pPipelineStateSubobjectStream) return false;
    const auto* base = static_cast<const uint8_t*>(desc->pPipelineStateSubobjectStream);
    size_t offset = 0;
    auto align = [](size_t v, size_t a) { return (v + a - 1) & ~(a - 1); };
    while (offset + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) <= desc->SizeInBytes) {
        const auto type = *reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE*>(base + offset);
        size_t size = 0, alignment = 4;
        switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: size = sizeof(void*); alignment = sizeof(void*); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
            size = sizeof(D3D12_SHADER_BYTECODE); alignment = alignof(D3D12_SHADER_BYTECODE); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: size = sizeof(D3D12_STREAM_OUTPUT_DESC); alignment = alignof(D3D12_STREAM_OUTPUT_DESC); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: size = sizeof(D3D12_BLEND_DESC); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: size = sizeof(UINT); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: size = sizeof(D3D12_RASTERIZER_DESC); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: size = sizeof(D3D12_DEPTH_STENCIL_DESC); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: size = sizeof(D3D12_INPUT_LAYOUT_DESC); alignment = alignof(D3D12_INPUT_LAYOUT_DESC); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: size = sizeof(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: size = sizeof(D3D12_PRIMITIVE_TOPOLOGY_TYPE); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: size = sizeof(D3D12_RT_FORMAT_ARRAY); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: size = sizeof(DXGI_FORMAT); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: size = sizeof(DXGI_SAMPLE_DESC); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: size = sizeof(UINT); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: size = sizeof(D3D12_CACHED_PIPELINE_STATE); alignment = alignof(D3D12_CACHED_PIPELINE_STATE); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: size = sizeof(D3D12_PIPELINE_STATE_FLAGS); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: size = sizeof(D3D12_DEPTH_STENCIL_DESC1); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: size = sizeof(D3D12_VIEW_INSTANCING_DESC); alignment = alignof(D3D12_VIEW_INSTANCING_DESC); break;
        default: return ps->pShaderBytecode != nullptr;  // Unknown subobject: stop, keep what we found.
        }
        const size_t payload = align(offset + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE), alignment);
        if (payload + size > desc->SizeInBytes) break;
        if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS) *ps = *reinterpret_cast<const D3D12_SHADER_BYTECODE*>(base + payload);
        if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE) *rootSig = *reinterpret_cast<void* const*>(base + payload);
        offset = align(payload + size, sizeof(void*));
    }
    return ps->pShaderBytecode != nullptr;
}

using CreateGraphicsPsoFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using CreatePipelineStateFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
using CreatePipelineLibraryFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const void*, SIZE_T, REFIID, void**);
using LoadGraphicsPipelineFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12PipelineLibrary*, LPCWSTR,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using LoadPipelineFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12PipelineLibrary1*, LPCWSTR,
    const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
constexpr size_t kLibraryLoadGraphicsPipeline = 9;
constexpr size_t kLibraryLoadPipeline = 13;
std::atomic<void*> g_libVtbl{nullptr};
void* g_libLoadGraphics = nullptr;
void* g_libLoadPipeline = nullptr;

HRESULT STDMETHODCALLTYPE HookedCreateGraphicsPipelineState(ID3D12Device* self,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** psoOut) {
    auto original = reinterpret_cast<CreateGraphicsPsoFn>(getDeviceOriginal(self, kSlotCreateGraphicsPipelineState));
    const HRESULT hr = original(self, desc, riid, psoOut);
    if (SUCCEEDED(hr) && desc && psoOut && *psoOut) recordPso("graphics", *psoOut, desc->pRootSignature, desc->PS);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedCreatePipelineState(ID3D12Device* self,
    const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid, void** psoOut) {
    auto original = reinterpret_cast<CreatePipelineStateFn>(getDeviceOriginal(self, kSlotCreatePipelineState));
    const HRESULT hr = original(self, desc, riid, psoOut);
    if (SUCCEEDED(hr) && psoOut && *psoOut) {
        D3D12_SHADER_BYTECODE ps{};
        void* rootSig = nullptr;
        if (parsePipelineStream(desc, &ps, &rootSig)) recordPso("stream", *psoOut, rootSig, ps);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedLoadGraphicsPipeline(ID3D12PipelineLibrary* self, LPCWSTR name,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** psoOut) {
    const HRESULT hr = reinterpret_cast<LoadGraphicsPipelineFn>(g_libLoadGraphics)(self, name, desc, riid, psoOut);
    if (SUCCEEDED(hr) && desc && psoOut && *psoOut) recordPso("library", *psoOut, desc->pRootSignature, desc->PS);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedLoadPipeline(ID3D12PipelineLibrary1* self, LPCWSTR name,
    const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid, void** psoOut) {
    const HRESULT hr = reinterpret_cast<LoadPipelineFn>(g_libLoadPipeline)(self, name, desc, riid, psoOut);
    if (SUCCEEDED(hr) && psoOut && *psoOut) {
        D3D12_SHADER_BYTECODE ps{};
        void* rootSig = nullptr;
        if (parsePipelineStream(desc, &ps, &rootSig)) recordPso("library_stream", *psoOut, rootSig, ps);
    }
    return hr;
}

bool writeVtableSlot(void** vtbl, size_t slot, void* hook);

HRESULT STDMETHODCALLTYPE HookedCreatePipelineLibrary(ID3D12Device* self, const void* blob, SIZE_T size,
    REFIID riid, void** libraryOut) {
    auto original = reinterpret_cast<CreatePipelineLibraryFn>(getDeviceOriginal(self, kSlotCreatePipelineLibrary));
    const HRESULT hr = original(self, blob, size, riid, libraryOut);
    if (SUCCEEDED(hr) && libraryOut && *libraryOut) {
        // All runtime pipeline libraries share one class vtable: patch it once (like the command lists).
        ID3D12PipelineLibrary1* lib1 = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*libraryOut)->QueryInterface(__uuidof(ID3D12PipelineLibrary1),
                reinterpret_cast<void**>(&lib1))) && lib1) {
            void** vt = *reinterpret_cast<void***>(lib1);
            void* expected = nullptr;
            if (g_libVtbl.compare_exchange_strong(expected, vt)) {
                g_libLoadGraphics = vt[kLibraryLoadGraphicsPipeline];
                g_libLoadPipeline = vt[kLibraryLoadPipeline];
                const bool ok = writeVtableSlot(vt, kLibraryLoadGraphicsPipeline, reinterpret_cast<void*>(&HookedLoadGraphicsPipeline)) &&
                    writeVtableSlot(vt, kLibraryLoadPipeline, reinterpret_cast<void*>(&HookedLoadPipeline));
                logLine("PIPELINE_LIBRARY vtbl=%p patched=%d blob_size=%llu", static_cast<void*>(vt), ok ? 1 : 0,
                    static_cast<unsigned long long>(size));
            }
            lib1->Release();
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedCreateDescriptorHeap(ID3D12Device* self,
    const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID riid, void** heapOut) {
    auto original = reinterpret_cast<CreateDescriptorHeapFn>(getDeviceOriginal(self, kSlotCreateDescriptorHeap));
    if (!original) return E_NOINTERFACE;
    const HRESULT hr = original(self, desc, riid, heapOut);
    if (SUCCEEDED(hr) && heapOut && *heapOut) {
        ID3D12DescriptorHeap* heap = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*heapOut)->QueryInterface(__uuidof(ID3D12DescriptorHeap),
                reinterpret_cast<void**>(&heap))) && heap) {
            trackHeap(self, heap);
            heap->Release();
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedCreateRootSignature(ID3D12Device* self, UINT nodeMask,
    const void* blob, SIZE_T size, REFIID riid, void** rootSigOut) {
    auto original = reinterpret_cast<CreateRootSignatureFn>(getDeviceOriginal(self, kSlotCreateRootSignature));
    if (!original) return E_NOINTERFACE;
    const HRESULT hr = original(self, nodeMask, blob, size, riid, rootSigOut);
    if (SUCCEEDED(hr) && rootSigOut && *rootSigOut) trackRootSignature(*rootSigOut, blob, size);
    return hr;
}

void STDMETHODCALLTYPE HookedCreateConstantBufferView(ID3D12Device* self,
    const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    reinterpret_cast<CreateCbvFn>(getDeviceOriginal(self, kSlotCreateConstantBufferView))(self, desc, dest);
    // CBV entries reuse the fields: resource = GPU virtual address, width = size in bytes.
    DescEntry e{};
    e.kind = kDescCbv;
    if (desc) {
        e.resource = reinterpret_cast<void*>(static_cast<uintptr_t>(desc->BufferLocation));
        e.width = desc->SizeInBytes;
    }
    AcquireSRWLockShared(&g_heapLock);
    if (DescEntry* slot = findEntryByCpuLocked(dest.ptr)) *slot = e;
    ReleaseSRWLockShared(&g_heapLock);
}

void STDMETHODCALLTYPE HookedCreateShaderResourceView(ID3D12Device* self, ID3D12Resource* resource,
    const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    reinterpret_cast<CreateSrvFn>(getDeviceOriginal(self, kSlotCreateShaderResourceView))(self, resource, desc, dest);
    recordView(dest.ptr, kDescSrv, resource, desc ? desc->Format : DXGI_FORMAT_UNKNOWN,
        desc ? static_cast<UINT>(desc->ViewDimension) : 0);
}

void STDMETHODCALLTYPE HookedCreateUnorderedAccessView(ID3D12Device* self, ID3D12Resource* resource,
    ID3D12Resource* counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    reinterpret_cast<CreateUavFn>(getDeviceOriginal(self, kSlotCreateUnorderedAccessView))(
        self, resource, counter, desc, dest);
    recordView(dest.ptr, kDescUav, resource, desc ? desc->Format : DXGI_FORMAT_UNKNOWN,
        desc ? static_cast<UINT>(desc->ViewDimension) : 0);
}

void STDMETHODCALLTYPE HookedCreateRenderTargetView(ID3D12Device* self, ID3D12Resource* resource,
    const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    reinterpret_cast<CreateRtvFn>(getDeviceOriginal(self, kSlotCreateRenderTargetView))(self, resource, desc, dest);
    recordView(dest.ptr, kDescRtv, resource, desc ? desc->Format : DXGI_FORMAT_UNKNOWN,
        desc ? static_cast<UINT>(desc->ViewDimension) : 0);
}

void STDMETHODCALLTYPE HookedCreateDepthStencilView(ID3D12Device* self, ID3D12Resource* resource,
    const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    reinterpret_cast<CreateDsvFn>(getDeviceOriginal(self, kSlotCreateDepthStencilView))(self, resource, desc, dest);
    recordView(dest.ptr, kDescDsv, resource, desc ? desc->Format : DXGI_FORMAT_UNKNOWN,
        desc ? static_cast<UINT>(desc->ViewDimension) : 0);
}

void STDMETHODCALLTYPE HookedCopyDescriptors(ID3D12Device* self, UINT numDest,
    const D3D12_CPU_DESCRIPTOR_HANDLE* destStarts, const UINT* destSizes, UINT numSrc,
    const D3D12_CPU_DESCRIPTOR_HANDLE* srcStarts, const UINT* srcSizes, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    reinterpret_cast<CopyDescriptorsFn>(getDeviceOriginal(self, kSlotCopyDescriptors))(
        self, numDest, destStarts, destSizes, numSrc, srcStarts, srcSizes, type);
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER || !destStarts || !srcStarts) return;
    const UINT inc = self->GetDescriptorHandleIncrementSize(type);
    AcquireSRWLockShared(&g_heapLock);
    UINT di = 0, doff = 0, si = 0, soff = 0;
    while (di < numDest && si < numSrc) {
        const UINT dsize = destSizes ? destSizes[di] : 1;
        const UINT ssize = srcSizes ? srcSizes[si] : 1;
        const UINT n = (dsize - doff) < (ssize - soff) ? (dsize - doff) : (ssize - soff);
        if (n) copyEntriesLocked(destStarts[di].ptr + static_cast<SIZE_T>(doff) * inc,
            srcStarts[si].ptr + static_cast<SIZE_T>(soff) * inc, n);
        doff += n; soff += n;
        if (doff >= dsize) { ++di; doff = 0; }
        if (soff >= ssize) { ++si; soff = 0; }
    }
    ReleaseSRWLockShared(&g_heapLock);
}

void STDMETHODCALLTYPE HookedCopyDescriptorsSimple(ID3D12Device* self, UINT num,
    D3D12_CPU_DESCRIPTOR_HANDLE dest, D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    reinterpret_cast<CopyDescriptorsSimpleFn>(getDeviceOriginal(self, kSlotCopyDescriptorsSimple))(
        self, num, dest, src, type);
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) return;
    AcquireSRWLockShared(&g_heapLock);
    copyEntriesLocked(dest.ptr, src.ptr, num);
    ReleaseSRWLockShared(&g_heapLock);
}

bool patchDevice(ID3D12Device* device, size_t requestedLength) {
    if (!device || requestedLength <= kSlotCreateReservedResource) return false;
    auto*** vtableAddress = reinterpret_cast<void***>(device);
    void** currentTable = *vtableAddress;

    AcquireSRWLockExclusive(&g_hookLock);
    auto existing = g_deviceOriginals.find(currentTable);
    if (existing != g_deviceOriginals.end() && existing->second.vtableLength >= requestedLength) {
        ReleaseSRWLockExclusive(&g_hookLock);
        return true;
    }

    // Clone the interface's full vtable and patch the per-device copy. This
    // avoids changing the D3D12 runtime's shared, read-only vtable.
    void** originalTable = existing == g_deviceOriginals.end()
        ? currentTable : existing->second.originalVtable;
    const size_t cloneLength = (existing == g_deviceOriginals.end())
        ? requestedLength : requestedLength;
    void** clone = static_cast<void**>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        cloneLength * sizeof(void*)));
    if (!clone) {
        ReleaseSRWLockExclusive(&g_hookLock);
        return false;
    }
    std::memcpy(clone, originalTable, cloneLength * sizeof(void*));

    DeviceOriginals originals;
    originals.originalVtable = originalTable;
    originals.vtableLength = cloneLength;
    std::memcpy(originals.slots, originalTable,
        (cloneLength < kDeviceOriginalSlots ? cloneLength : kDeviceOriginalSlots) * sizeof(void*));
    clone[kSlotQueryInterface] = reinterpret_cast<void*>(&HookedQueryInterface);
    // Shader capture (F8 dump) only in the probe build. Device1/Device2 slots exist only when the interface is new
    // enough (lengths from deviceVtableLength).
    if (!kRelease) clone[kSlotCreateGraphicsPipelineState] = reinterpret_cast<void*>(&HookedCreateGraphicsPipelineState);
    if (!kRelease && cloneLength > kSlotCreatePipelineState) {
        clone[kSlotCreatePipelineLibrary] = reinterpret_cast<void*>(&HookedCreatePipelineLibrary);
        clone[kSlotCreatePipelineState] = reinterpret_cast<void*>(&HookedCreatePipelineState);
    }
    clone[kSlotCreateDescriptorHeap] = reinterpret_cast<void*>(&HookedCreateDescriptorHeap);
    clone[kSlotCreateRootSignature] = reinterpret_cast<void*>(&HookedCreateRootSignature);
    clone[kSlotCreateConstantBufferView] = reinterpret_cast<void*>(&HookedCreateConstantBufferView);
    clone[kSlotCreateShaderResourceView] = reinterpret_cast<void*>(&HookedCreateShaderResourceView);
    clone[kSlotCreateUnorderedAccessView] = reinterpret_cast<void*>(&HookedCreateUnorderedAccessView);
    clone[kSlotCreateRenderTargetView] = reinterpret_cast<void*>(&HookedCreateRenderTargetView);
    clone[kSlotCreateDepthStencilView] = reinterpret_cast<void*>(&HookedCreateDepthStencilView);
    clone[kSlotCopyDescriptors] = reinterpret_cast<void*>(&HookedCopyDescriptors);
    clone[kSlotCopyDescriptorsSimple] = reinterpret_cast<void*>(&HookedCopyDescriptorsSimple);
    clone[kSlotCreateCommittedResource] = reinterpret_cast<void*>(&HookedCreateCommittedResource);
    clone[kSlotCreatePlacedResource] = reinterpret_cast<void*>(&HookedCreatePlacedResource);
    clone[kSlotCreateReservedResource] = reinterpret_cast<void*>(&HookedCreateReservedResource);
    try {
        g_deviceOriginals.emplace(clone, originals);
    } catch (...) {
        HeapFree(GetProcessHeap(), 0, clone);
        ReleaseSRWLockExclusive(&g_hookLock);
        return false;
    }

    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(vtableAddress), clone);
    logLine("DEVICE_VTABLE_CLONED device=%p old=%p clone=%p entries=%llu",
        device, originalTable, clone, static_cast<unsigned long long>(cloneLength));
    ReleaseSRWLockExclusive(&g_hookLock);
    return true;
}


// ============================================================================
// Frame trace. Patches the D3D12 command-list and command-queue vtables in
// place (slot patching) and, when F9 is pressed, records ~150 ms of GPU work:
// dispatch sizes, draws, pipeline-state changes, root signatures, the textures
// behind every descriptor-table bind and render-target set, texture barriers,
// copies and the order command lists are submitted. Written to d3d12_trace_N.log.
// ============================================================================
#pragma comment(lib, "user32.lib")

constexpr size_t kListClose = 9;
constexpr size_t kListReset = 10;
constexpr size_t kListDrawInstanced = 12;
constexpr size_t kListDrawIndexedInstanced = 13;
constexpr size_t kListDispatch = 14;
constexpr size_t kListCopyResource = 17;
constexpr size_t kListSetPipelineState = 25;
constexpr size_t kListResourceBarrier = 26;
constexpr size_t kListSetComputeRootSignature = 29;
constexpr size_t kListSetGraphicsRootSignature = 30;
constexpr size_t kListSetComputeRootDescriptorTable = 31;
constexpr size_t kListSetGraphicsRootDescriptorTable = 32;
constexpr size_t kListOMSetRenderTargets = 46;
constexpr size_t kListExecuteIndirect = 59;
constexpr size_t kListSetDescriptorHeaps = 28;
constexpr size_t kQueueExecuteCommandLists = 10;
constexpr UINT kMaxDescriptorsPerTableLog = 64;
constexpr UINT kMinLoggedDescriptorDim = 256;
constexpr int kMaxTables = 8;
constexpr long long kCaptureWindowMs = 150;
constexpr size_t kMaxCaptureBytes = 96u * 1024u * 1024u;

using CloseFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using DispatchFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
using CopyResourceFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
using SetPsoFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using BarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
using SetRootSigFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
using SetTableFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT,
    const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
using ExecuteIndirectFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandSignature*,
    UINT, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64);
using SetDescriptorHeapsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);

void afterGameDispatch(ID3D12GraphicsCommandList* list, UINT x, UINT y);

struct ListTable {
    void** vtbl = nullptr;
    void* close = nullptr; void* reset = nullptr; void* dispatch = nullptr;
    void* copyResource = nullptr; void* setPso = nullptr; void* barrier = nullptr;
    void* draw = nullptr; void* drawIndexed = nullptr;
    void* setComputeRs = nullptr; void* setGraphicsRs = nullptr;
    void* setComputeTable = nullptr; void* setGraphicsTable = nullptr;
    void* omSetRenderTargets = nullptr; void* executeIndirect = nullptr;
    void* setDescriptorHeaps = nullptr;
};

// Root signatures, descriptor tables and descriptor heaps bound per command list. A list is recorded
// by one thread between Reset and Close, so a small per-thread cache is enough (no locks).
// Used to resolve bindings and to restore the game's state after our own passes.
constexpr UINT kTrackedTableParams = 8;

// Per-frame values captured by the FSR 4 MV pass at the TAA bind, consumed at FSR 1's RCAS dispatch.
struct PrepData {
    float jitter[2];
    float nearPlane, farPlane, fovY, frameTimeMs;
    bool reset;
    bool hdr;          // TAA colour was HDR RGBA16F (game FSR off) instead of tone-mapped RGBA8.
    LONGLONG qpc;
    unsigned seq;
};

struct ListRootSigs {
    void* list; void* compute; void* graphics;
    UINT64 graphicsTables[kTrackedTableParams];   // Last graphics table base per root parameter (0 = unset).
    UINT64 computeTables[kTrackedTableParams];    // Same for compute.
    ID3D12DescriptorHeap* heaps[2];
    UINT heapCount;
    ID3D12PipelineState* pso;                     // Last pipeline state set.
    bool fsrPending;                              // This list holds a prepared FSR 4 frame (MV pass recorded here).
    PrepData fsrPrep;
    UINT64 taaTable;                              // Stock TAA table bound at the prep (graphics root param taaParam).
    UINT taaParam;
    UINT taaDrawsSkipped;                         // Stock TAA draws skipped since the prep (max 3).
    bool easuSkipped;                             // FSR 1 EASU skipped since the prep.
    SIZE_T rt0Cpu;                                // RTV of render target 0 (OMSetRenderTargets): the TAA output.
    bool nativeFrame;                             // Prep made in native mode (no FSR 1 in the game): FSR 4 at the TAA draws.
    bool nativeDone;                              // Native FSR 4 already written this frame; remaining TAA draws dropped.
    bool overlayPending;                          // FSR 4 off: draw the notification after the stock TAA draws.
    UINT overlayDraws;
};

void afterGameDraw(ID3D12GraphicsCommandList* list);

bool skipTaaDraw(ID3D12GraphicsCommandList* list);
bool beforeGameDispatch(ID3D12GraphicsCommandList* list, UINT x, UINT y);

void fsrOnTaaBind(ID3D12GraphicsCommandList* list, UINT param, D3D12_GPU_DESCRIPTOR_HANDLE base);
void pollFsrKey();
void onSubmitForAb(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
constexpr int kListRsCacheSize = 16;
thread_local ListRootSigs t_listRs[kListRsCacheSize];
thread_local int t_listRsNext = 0;
// Set while we record our own commands (MV pass, FFX) so the hooks don't react to them.
thread_local bool t_inOwnWork = false;

ListRootSigs* listRootSigs(void* list, bool create) {
    for (ListRootSigs& e : t_listRs) if (e.list == list) return &e;
    if (!create) return nullptr;
    ListRootSigs& e = t_listRs[t_listRsNext];
    t_listRsNext = (t_listRsNext + 1) % kListRsCacheSize;
    e = ListRootSigs{};
    e.list = list;
    return &e;
}
struct QueueTable { void** vtbl = nullptr; void* execute = nullptr; };

ListTable g_listTables[kMaxTables];
QueueTable g_queueTables[kMaxTables];
std::atomic<int> g_listTableCount{0};
std::atomic<int> g_queueTableCount{0};
SRWLOCK g_tableLock = SRWLOCK_INIT;

std::atomic<bool> g_capBusy{false};
std::atomic<bool> g_capArmed{false};
std::atomic<long long> g_capEnd{0};
std::atomic<unsigned> g_capIndex{0};
std::atomic<bool> g_f9Down{false};
std::string g_capBuf;
SRWLOCK g_capLock = SRWLOCK_INIT;
std::atomic<bool> g_firedClose{false}, g_firedReset{false}, g_firedDispatch{false},
    g_firedCopy{false}, g_firedPso{false}, g_firedBarrier{false}, g_firedExecute{false};

void noteFired(std::atomic<bool>& flag, const char* name) {
    if (!flag.exchange(true)) logLine("TRACE_HOOK_FIRED %s", name);
}

const ListTable* findListTable(void* self) {
    void** vt = *reinterpret_cast<void***>(self);
    const int n = g_listTableCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) if (g_listTables[i].vtbl == vt) return &g_listTables[i];
    return nullptr;
}
const QueueTable* findQueueTable(void* self) {
    void** vt = *reinterpret_cast<void***>(self);
    const int n = g_queueTableCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) if (g_queueTables[i].vtbl == vt) return &g_queueTables[i];
    return nullptr;
}

void traceLine(const char* format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    const int count = vsnprintf(line, sizeof(line) - 1, format, args);
    va_end(args);
    if (count < 0) return;
    const size_t length = static_cast<size_t>(count) < sizeof(line) - 1
        ? static_cast<size_t>(count) : sizeof(line) - 2;
    line[length] = '\n';
    AcquireSRWLockExclusive(&g_capLock);
    if (g_capBuf.size() < kMaxCaptureBytes) g_capBuf.append(line, length + 1);
    ReleaseSRWLockExclusive(&g_capLock);
}

void finishCapture() {
    bool expected = true;
    if (!g_capArmed.compare_exchange_strong(expected, false)) return;
    std::string out;
    AcquireSRWLockExclusive(&g_capLock);
    out.swap(g_capBuf);
    ReleaseSRWLockExclusive(&g_capLock);
    const unsigned index = g_capIndex.fetch_add(1) + 1;

    wchar_t path[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", path, ARRAYSIZE(path));
    if (length != 0 && length < ARRAYSIZE(path) - 40 &&
        SUCCEEDED(StringCchCatW(path, ARRAYSIZE(path), L"\\ACVFSR31"))) {
        CreateDirectoryW(path, nullptr);
        wchar_t name[48]{};
        StringCchPrintfW(name, ARRAYSIZE(name), L"\\d3d12_trace_%u.log", index);
        if (SUCCEEDED(StringCchCatW(path, ARRAYSIZE(path), name))) {
            const HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                size_t offset = 0;
                while (offset < out.size()) {
                    const DWORD chunk = static_cast<DWORD>(
                        (out.size() - offset) > (1u << 24) ? (1u << 24) : (out.size() - offset));
                    DWORD written = 0;
                    if (!WriteFile(file, out.data() + offset, chunk, &written, nullptr) || written == 0) break;
                    offset += written;
                }
                CloseHandle(file);
            }
        }
    }
    logLine("TRACE_CAPTURE_WRITTEN index=%u bytes=%llu", index, static_cast<unsigned long long>(out.size()));
    MessageBeep(MB_OK);
    g_capBusy.store(false);
}

void armCapture() {
    bool expected = false;
    if (!g_capBusy.compare_exchange_strong(expected, true)) return;
    LARGE_INTEGER freq{}, now{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    AcquireSRWLockExclusive(&g_capLock);
    g_capBuf.clear();
    g_capBuf.reserve(32u * 1024u * 1024u);
    ReleaseSRWLockExclusive(&g_capLock);
    g_capEnd.store(now.QuadPart + freq.QuadPart * kCaptureWindowMs / 1000);
    g_capArmed.store(true, std::memory_order_release);
    traceLine("TRACE_BEGIN window_ms=%lld qpc_freq=%lld", kCaptureWindowMs,
        static_cast<long long>(freq.QuadPart));
    logLine("TRACE_CAPTURE_ARMED");
    MessageBeep(MB_ICONASTERISK);
}

inline bool capturing() {
    if (!g_capArmed.load(std::memory_order_acquire)) return false;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (now.QuadPart < g_capEnd.load(std::memory_order_relaxed)) return true;
    finishCapture();
    return false;
}

bool describeTexture(ID3D12Resource* resource, char* out, size_t outSize) {
    if (!resource) return false;
    const D3D12_RESOURCE_DESC d = resource->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width < 256 || d.Height < 256) return false;
    snprintf(out, outSize, "%p %llux%u f%u fl%X m%u a%u", static_cast<void*>(resource),
        static_cast<unsigned long long>(d.Width), d.Height, static_cast<unsigned>(d.Format),
        static_cast<unsigned>(d.Flags), d.MipLevels, d.DepthOrArraySize);
    return true;
}

void traceBarrier(void* list, const D3D12_RESOURCE_BARRIER& b) {
    char a[160], c[160];
    const unsigned long tid = GetCurrentThreadId();
    switch (b.Type) {
    case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
        if (describeTexture(b.Transition.pResource, a, sizeof(a))) {
            traceLine("T%lu L%p BAR_T %s sub=%u %X->%X fl=%X", tid, list, a,
                b.Transition.Subresource, static_cast<unsigned>(b.Transition.StateBefore),
                static_cast<unsigned>(b.Transition.StateAfter), static_cast<unsigned>(b.Flags));
        }
        break;
    case D3D12_RESOURCE_BARRIER_TYPE_UAV:
        if (describeTexture(b.UAV.pResource, a, sizeof(a))) traceLine("T%lu L%p BAR_U %s", tid, list, a);
        break;
    case D3D12_RESOURCE_BARRIER_TYPE_ALIASING: {
        const bool hasBefore = describeTexture(b.Aliasing.pResourceBefore, a, sizeof(a));
        const bool hasAfter = describeTexture(b.Aliasing.pResourceAfter, c, sizeof(c));
        if (hasBefore || hasAfter) {
            traceLine("T%lu L%p BAR_A before=[%s] after=[%s]", tid, list, hasBefore ? a : "-", hasAfter ? c : "-");
        }
        break;
    }
    default: break;
    }
}

void notePsoForDump(void* list, void* pso);
void noteCloseForDump(void* list);

HRESULT STDMETHODCALLTYPE HookedClose(ID3D12GraphicsCommandList* self) {
    noteFired(g_firedClose, "Close");
    noteCloseForDump(self);
    if (capturing()) traceLine("T%lu L%p CLOSE", GetCurrentThreadId(), static_cast<void*>(self));
    return reinterpret_cast<CloseFn>(findListTable(self)->close)(self);
}
HRESULT STDMETHODCALLTYPE HookedReset(ID3D12GraphicsCommandList* self,
    ID3D12CommandAllocator* allocator, ID3D12PipelineState* pso) {
    noteFired(g_firedReset, "Reset");
    if (ListRootSigs* rs = listRootSigs(self, false)) { *rs = ListRootSigs{}; rs->list = self; }
    if (capturing()) traceLine("T%lu L%p RESET", GetCurrentThreadId(), static_cast<void*>(self));
    return reinterpret_cast<ResetFn>(findListTable(self)->reset)(self, allocator, pso);
}
void STDMETHODCALLTYPE HookedDispatch(ID3D12GraphicsCommandList* self, UINT x, UINT y, UINT z) {
    noteFired(g_firedDispatch, "Dispatch");
    if (capturing()) traceLine("T%lu L%p DISP %u %u %u", GetCurrentThreadId(), static_cast<void*>(self), x, y, z);
    if (!t_inOwnWork && beforeGameDispatch(self, x, y)) return;   // FSR 1 pass skipped (and RCAS replaced by FSR 4).
    reinterpret_cast<DispatchFn>(findListTable(self)->dispatch)(self, x, y, z);
    if (!t_inOwnWork) afterGameDispatch(self, x, y);
}
void STDMETHODCALLTYPE HookedCopyResource(ID3D12GraphicsCommandList* self,
    ID3D12Resource* dst, ID3D12Resource* src) {
    noteFired(g_firedCopy, "CopyResource");
    if (capturing()) {
        char a[160], b[160];
        const bool d = describeTexture(dst, a, sizeof(a));
        const bool s = describeTexture(src, b, sizeof(b));
        if (d || s) traceLine("T%lu L%p COPY dst=[%s] src=[%s]", GetCurrentThreadId(),
            static_cast<void*>(self), d ? a : "-", s ? b : "-");
    }
    reinterpret_cast<CopyResourceFn>(findListTable(self)->copyResource)(self, dst, src);
}
void STDMETHODCALLTYPE HookedSetPipelineState(ID3D12GraphicsCommandList* self, ID3D12PipelineState* pso) {
    noteFired(g_firedPso, "SetPipelineState");
    notePsoForDump(self, pso);
    listRootSigs(self, true)->pso = pso;
    if (capturing()) traceLine("T%lu L%p PSO %p", GetCurrentThreadId(), static_cast<void*>(self), static_cast<void*>(pso));
    reinterpret_cast<SetPsoFn>(findListTable(self)->setPso)(self, pso);
}
void STDMETHODCALLTYPE HookedResourceBarrier(ID3D12GraphicsCommandList* self, UINT count,
    const D3D12_RESOURCE_BARRIER* barriers) {
    noteFired(g_firedBarrier, "ResourceBarrier");
    if (barriers && capturing()) {
        for (UINT i = 0; i < count; ++i) traceBarrier(self, barriers[i]);
    }
    reinterpret_cast<BarrierFn>(findListTable(self)->barrier)(self, count, barriers);
}
void pollDumpKey();
void onSubmitForDump(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);

void STDMETHODCALLTYPE HookedExecuteCommandLists(ID3D12CommandQueue* self, UINT count,
    ID3D12CommandList* const* lists) {
    noteFired(g_firedExecute, "ExecuteCommandLists");
    pollDumpKey();
    pollFsrKey();
    if (!kRelease) {   // F9 frame trace: probe build only.
        const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (down) { if (!g_f9Down.exchange(true)) armCapture(); }
        else g_f9Down.store(false);
    }
    if (lists && capturing()) {
        char buf[400];
        int off = snprintf(buf, sizeof(buf), "T%lu EXEC q=%p n=%u lists=", GetCurrentThreadId(),
            static_cast<void*>(self), count);
        for (UINT i = 0; i < count && i < 24 && off < 340; ++i) {
            off += snprintf(buf + off, sizeof(buf) - off, "%p,", static_cast<void*>(lists[i]));
        }
        traceLine("%s", buf);
    }
    reinterpret_cast<ExecuteFn>(findQueueTable(self)->execute)(self, count, lists);
    if (lists) onSubmitForDump(self, count, lists);
    if (lists) onSubmitForAb(self, count, lists);
}

std::atomic<bool> g_firedDraw{false}, g_firedRootSig{false}, g_firedTable{false},
    g_firedRenderTargets{false}, g_firedIndirect{false};

// ============================================================================
// F8 texture dump. When armed, the next graphics descriptor-table bind whose
// SRVs match the stock TAA/resolve input layout (RGBA8, RGBA8, R32F, R8, RGBA8,
// R8, RGBA8, all one size) gets copy commands recorded into the game's own
// command list: state C0 -> COPY_SOURCE, CopyTextureRegion into readback
// buffers, COPY_SOURCE -> C0. When that list is submitted we signal our fence
// on the game's queue; a worker waits for it and writes
// %LOCALAPPDATA%\ACVFSR31\dump_N\slot<i>_f<fmt>_<w>x<h>.raw (rows tightly
// packed) plus dump.txt.
// ============================================================================
// Stock TAA inputs t0..t6 (colour, packed MV, linear depth, R8 mask, history, R8 history, pre-pass). Two variants:
// game FSR on  -> TAA after tone mapping, 8-bit colour/history (RGBA8_UNORM = 28);
// game FSR off -> TAA before tone mapping (native 4K), HDR colour/history (RGBA16_FLOAT = 10). MV/depth/masks are the same.
constexpr UINT kTaaInputCount = 7;
constexpr uint16_t kTaaInputFormatsLdr[kTaaInputCount] = {28, 28, 41, 61, 28, 61, 28};
constexpr uint16_t kTaaInputFormatsHdr[kTaaInputCount] = {10, 28, 41, 61, 10, 61, 10};
const uint16_t* const kTaaInputFormatSets[] = {kTaaInputFormatsLdr, kTaaInputFormatsHdr};
constexpr UINT kTaaInputMinWidth = 1280;
constexpr D3D12_RESOURCE_STATES kTaaInputState =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr ULONGLONG kDumpSubmitTimeoutMs = 5000;
constexpr UINT kTaaPsoCount = 3;
constexpr unsigned kCbCaptureFrames = 16;
constexpr size_t kCbMaxBytes = 4096;
constexpr UINT kTaaViewConstantsParam = 1;   // Root param holding the space1 view constants table (from rootsig.txt).
std::atomic<int> g_cbFramesLeft{0};
SRWLOCK g_cbLock = SRWLOCK_INIT;
std::string g_cbLog;

struct DumpJob {
    unsigned index = 0;
    void* list = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64 fenceValue = 0;
    ULONGLONG recordedTick = 0;
    ID3D12Resource* readback[kTaaInputCount]{};
    void* source[kTaaInputCount]{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint[kTaaInputCount]{};
    UINT rows[kTaaInputCount]{};
    UINT64 rowBytes[kTaaInputCount]{};
    uint16_t format[kTaaInputCount]{};
    void* rootSig = nullptr;              // Graphics root signature bound when the table matched.
    void* pso[kTaaPsoCount]{};            // Pipelines set on the list right after the match (the TAA draws).
    UINT psoCount = 0;
};

// The job whose list is still being recorded on this thread (to catch its next SetPipelineState calls).
thread_local DumpJob* t_psoJob = nullptr;

void notePsoForDump(void* list, void* pso) {
    DumpJob* job = t_psoJob;
    if (!job || job->list != list) return;
    if (job->psoCount < kTaaPsoCount) job->pso[job->psoCount++] = pso;
    if (job->psoCount == kTaaPsoCount) t_psoJob = nullptr;
}

void noteCloseForDump(void* list) {
    if (t_psoJob && t_psoJob->list == list) t_psoJob = nullptr;
}

// Writes the TAA pixel shaders (raw bytecode containers) and root signature description next to the textures.
void writeDumpShaders(const DumpJob* job, const wchar_t* dir, std::string& meta) {
    char line[256];
    std::string rootSigText;
    for (UINT k = 0; k < job->psoCount; ++k) {
        std::string bytecode;
        uint64_t hash = 0;
        void* psoRootSig = nullptr;
        AcquireSRWLockShared(&g_shaderLock);
        auto it = g_psoPixelShader.find(job->pso[k]);
        if (it != g_psoPixelShader.end()) {
            hash = it->second;
            auto blob = g_pixelShaders.find(hash);
            if (blob != g_pixelShaders.end()) {
                try { bytecode = blob->second; } catch (...) {}
            }
            auto rs = g_psoRootSig.find(job->pso[k]);
            if (rs != g_psoRootSig.end()) psoRootSig = rs->second;
        }
        ReleaseSRWLockShared(&g_shaderLock);
        snprintf(line, sizeof(line), "pso%u=%p ps_hash=%016llX ps_bytes=%llu pso_rootsig=%p\r\n", k, job->pso[k],
            static_cast<unsigned long long>(hash), static_cast<unsigned long long>(bytecode.size()), psoRootSig);
        meta += line;
        if (bytecode.empty()) continue;
        wchar_t path[MAX_PATH]{};
        StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\ps%u_%016llX.bin", dir, k, static_cast<unsigned long long>(hash));
        const HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, bytecode.data(), static_cast<DWORD>(bytecode.size()), &written, nullptr);
            CloseHandle(file);
        }
    }
    std::string blob;
    AcquireSRWLockShared(&g_rsLock);
    auto it = g_rootSigBlobs.find(job->rootSig);
    if (it != g_rootSigBlobs.end()) {
        try { blob = it->second; } catch (...) {}
    }
    ReleaseSRWLockShared(&g_rsLock);
    snprintf(line, sizeof(line), "rootsig=%p blob_bytes=%llu pipelines_seen=%lu unique_pixel_shaders=%llu\r\n", job->rootSig,
        static_cast<unsigned long long>(blob.size()), g_psoCreated.load(),
        static_cast<unsigned long long>(g_pixelShaders.size()));
    meta += line;
    rootSigText = describeRootSignature(blob);
    wchar_t path[MAX_PATH]{};
    StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\rootsig.txt", dir);
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, rootSigText.data(), static_cast<DWORD>(rootSigText.size()), &written, nullptr);
        CloseHandle(file);
    }
}

std::atomic<bool> g_dumpArmed{false};
std::atomic<bool> g_f8Down{false};
std::atomic<DumpJob*> g_dumpRecorded{nullptr};   // Recorded, waiting for its list to be submitted.
std::atomic<bool> g_dumpBusy{false};              // A job exists (recorded or being written).
std::atomic<unsigned> g_dumpIndex{0};
ID3D12Fence* g_dumpFence = nullptr;               // Created on first dump; guarded by g_dumpBusy.
UINT64 g_dumpFenceValue = 0;

void freeDumpJob(DumpJob* job) {
    for (ID3D12Resource* r : job->readback) if (r) r->Release();
    delete job;
}

DWORD WINAPI dumpWriterThread(LPVOID param) {
    DumpJob* job = static_cast<DumpJob*>(param);
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool ready = false;
    if (event && SUCCEEDED(job->fence->SetEventOnCompletion(job->fenceValue, event))) {
        ready = WaitForSingleObject(event, 10000) == WAIT_OBJECT_0;
    }
    if (event) CloseHandle(event);
    if (!ready) {
        logLine("DUMP index=%u error=gpu_timeout", job->index);
    } else {
        wchar_t dir[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, ARRAYSIZE(dir));
        wchar_t sub[32]{};
        StringCchPrintfW(sub, ARRAYSIZE(sub), L"\\ACVFSR31\\dump_%u", job->index);
        if (length != 0 && length < ARRAYSIZE(dir) - 40 && SUCCEEDED(StringCchCatW(dir, ARRAYSIZE(dir), sub))) {
            CreateDirectoryW(dir, nullptr);
            std::string meta;
            for (UINT i = 0; i < kTaaInputCount; ++i) {
                const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp = job->footprint[i];
                wchar_t path[MAX_PATH]{};
                StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\slot%u_f%u_%ux%u.raw", dir, i,
                    job->format[i], fp.Footprint.Width, fp.Footprint.Height);
                void* mapped = nullptr;
                const D3D12_RANGE readRange{0, static_cast<SIZE_T>(fp.Footprint.RowPitch) * job->rows[i]};
                if (FAILED(job->readback[i]->Map(0, &readRange, &mapped)) || !mapped) continue;
                const HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file != INVALID_HANDLE_VALUE) {
                    for (UINT row = 0; row < job->rows[i]; ++row) {
                        DWORD written = 0;
                        WriteFile(file, static_cast<const uint8_t*>(mapped) + static_cast<SIZE_T>(row) * fp.Footprint.RowPitch,
                            static_cast<DWORD>(job->rowBytes[i]), &written, nullptr);
                    }
                    CloseHandle(file);
                }
                const D3D12_RANGE noWrite{0, 0};
                job->readback[i]->Unmap(0, &noWrite);
                char line[160];
                snprintf(line, sizeof(line), "slot=%u src=%p format=%u width=%u height=%u row_bytes=%llu\r\n", i,
                    job->source[i], job->format[i], fp.Footprint.Width, fp.Footprint.Height,
                    static_cast<unsigned long long>(job->rowBytes[i]));
                meta += line;
            }
            writeDumpShaders(job, dir, meta);
            wchar_t metaPath[MAX_PATH]{};
            StringCchPrintfW(metaPath, ARRAYSIZE(metaPath), L"%s\\dump.txt", dir);
            const HANDLE file = CreateFileW(metaPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(file, meta.data(), static_cast<DWORD>(meta.size()), &written, nullptr);
                CloseHandle(file);
            }
            logLine("DUMP index=%u written", job->index);
        }
    }
    MessageBeep(MB_OK);
    freeDumpJob(job);
    g_dumpBusy.store(false);
    return 0;
}

// Reads the first CBV of a bound descriptor table (CPU read of its upload buffer). Returns bytes read.
size_t readTableCbv(void* rootSig, UINT param, UINT64 gpuBase, uint8_t* out, size_t maxBytes, UINT64* vaOut) {
    *vaOut = 0;
    UINT offset = UINT_MAX;
    AcquireSRWLockShared(&g_rsLock);
    auto it = g_rootSigs.find(rootSig);
    if (it != g_rootSigs.end() && param < it->second.size()) {
        for (const RsRange& r : it->second[param].ranges) if (r.kind == kDescCbv) { offset = r.offset; break; }
    }
    ReleaseSRWLockShared(&g_rsLock);
    if (offset == UINT_MAX || !gpuBase) return 0;
    UINT64 va = 0, size = 0;
    AcquireSRWLockShared(&g_heapLock);
    UINT index = 0;
    const HeapRecord* heap = findHeapByGpuLocked(gpuBase, &index);
    if (heap && static_cast<UINT64>(index) + offset < heap->count) {
        const DescEntry& e = heap->entries[index + offset];
        if (e.kind == kDescCbv) { va = reinterpret_cast<uintptr_t>(e.resource); size = e.width; }
    }
    ReleaseSRWLockShared(&g_heapLock);
    *vaOut = va;
    return va ? readUploadMemory(va, static_cast<size_t>(size), out, maxBytes) : 0;
}

void appendCbDump(std::string& log, const char* name, UINT64 va, const uint8_t* data, size_t bytes) {
    char line[256];
    snprintf(line, sizeof(line), "%s va=0x%llX bytes=%llu\r\n", name, static_cast<unsigned long long>(va),
        static_cast<unsigned long long>(bytes));
    log += line;
    for (size_t i = 0; i + 16 <= bytes; i += 16) {
        float f[4];
        uint32_t u[4];
        std::memcpy(f, data + i, 16);
        std::memcpy(u, data + i, 16);
        snprintf(line, sizeof(line), "  [%3llu] % .7g % .7g % .7g % .7g | %08X %08X %08X %08X\r\n",
            static_cast<unsigned long long>(i / 16), f[0], f[1], f[2], f[3], u[0], u[1], u[2], u[3]);
        log += line;
    }
}

// Logs the TAA pass constants (CB1 = this table's CBV, space2) and the view constants (CB0 = table at root param 1,
// space1) for kCbCaptureFrames frames after F8; writes %LOCALAPPDATA%\ACVFSR31\cb_frames.txt at the end.
void captureTaaConstants(ID3D12GraphicsCommandList* list, void* rootSig, UINT param, D3D12_GPU_DESCRIPTOR_HANDLE base) {
    const int left = g_cbFramesLeft.fetch_sub(1);
    if (left <= 0) return;
    const ListRootSigs* bound = listRootSigs(list, false);
    uint8_t cb1[kCbMaxBytes], cb0[kCbMaxBytes];
    UINT64 va1 = 0, va0 = 0;
    const size_t n1 = readTableCbv(rootSig, param, base.ptr, cb1, sizeof(cb1), &va1);
    const UINT viewParam = kTaaViewConstantsParam;
    const size_t n0 = bound && viewParam < kTrackedTableParams
        ? readTableCbv(rootSig, viewParam, bound->graphicsTables[viewParam], cb0, sizeof(cb0), &va0) : 0;
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    std::string entry;
    char line[160];
    const unsigned frame = kCbCaptureFrames - static_cast<unsigned>(left);
    snprintf(line, sizeof(line), "=== frame %u t_ms=%.3f list=%p rootsig=%p table_param=%u\r\n", frame,
        1000.0 * static_cast<double>(now.QuadPart) / static_cast<double>(freq.QuadPart), static_cast<void*>(list), rootSig, param);
    entry += line;
    appendCbDump(entry, "CB1 (space2 b0, TAA pass)", va1, cb1, n1);
    appendCbDump(entry, "CB0 (space1 b0, view)", va0, cb0, n0);
    AcquireSRWLockExclusive(&g_cbLock);
    try { g_cbLog += entry; } catch (...) {}
    std::string finished;
    if (left == 1) finished.swap(g_cbLog);
    ReleaseSRWLockExclusive(&g_cbLock);
    if (left != 1) return;

    wchar_t path[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", path, ARRAYSIZE(path));
    if (length == 0 || length >= ARRAYSIZE(path) - 40 ||
        FAILED(StringCchCatW(path, ARRAYSIZE(path), L"\\ACVFSR31\\cb_frames.txt"))) return;
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, finished.data(), static_cast<DWORD>(finished.size()), &written, nullptr);
        CloseHandle(file);
    }
    logLine("CB_CAPTURE written frames=%u bytes=%llu", kCbCaptureFrames, static_cast<unsigned long long>(finished.size()));
}

// Distinct root signatures the TAA table has been matched on (logging only; the game recreates root
// signatures when a level loads and reuses addresses, so a match must never be cached by pointer).
SRWLOCK g_taaSeenLock = SRWLOCK_INIT;
void* g_taaSeenRootSigs[16]{};
UINT g_taaSeenCount = 0;

// True when the graphics table being bound holds the stock TAA inputs (format run RGBA8,RGBA8,R32F,R8,RGBA8,R8,RGBA8,
// one size); fills taa[] with their descriptor entries (t0..t6 of the TAA shader) and the bound root signature.
// Checked on every bind of a table with >= 7 SRVs; tables with fewer are rejected from the root-signature layout alone.
bool matchTaaInputs(ID3D12GraphicsCommandList* list, UINT param, D3D12_GPU_DESCRIPTOR_HANDLE base,
    DescEntry taa[kTaaInputCount], void** rootSigOut) {
    const ListRootSigs* bound = listRootSigs(list, false);
    void* rootSig = bound ? bound->graphics : nullptr;
    if (!rootSig) return false;

    // Collect the table's texture SRVs in descriptor order (root sig lock, then heap lock).
    DescEntry srvs[kMaxDescriptorsPerTableLog]{};
    UINT srvCount = 0;
    AcquireSRWLockShared(&g_rsLock);
    auto it = g_rootSigs.find(rootSig);
    if (it != g_rootSigs.end() && param < it->second.size() && it->second[param].table &&
        it->second[param].srvCount >= kTaaInputCount) {
        AcquireSRWLockShared(&g_heapLock);
        UINT index = 0;
        const HeapRecord* heap = findHeapByGpuLocked(base.ptr, &index);
        if (heap) {
            for (const RsRange& range : it->second[param].ranges) {
                if (range.kind != kDescSrv) continue;
                for (UINT j = 0; j < range.count && srvCount < kMaxDescriptorsPerTableLog; ++j) {
                    const UINT64 slot = static_cast<UINT64>(index) + range.offset + j;
                    if (slot >= heap->count) break;
                    const DescEntry& e = heap->entries[slot];
                    if (e.kind == kDescSrv && e.resource && e.resDim == D3D12_RESOURCE_DIMENSION_TEXTURE2D) srvs[srvCount++] = e;
                }
            }
        }
        ReleaseSRWLockShared(&g_heapLock);
    }
    ReleaseSRWLockShared(&g_rsLock);

    for (UINT start = 0; srvCount >= kTaaInputCount && start + kTaaInputCount <= srvCount; ++start) {
        bool match = false;
        for (const uint16_t* formats : kTaaInputFormatSets) {
            match = srvs[start].width >= kTaaInputMinWidth;
            for (UINT k = 0; match && k < kTaaInputCount; ++k) {
                const DescEntry& e = srvs[start + k];
                match = e.resFormat == formats[k] && e.width == srvs[start].width &&
                    e.height == srvs[start].height && e.arraySize == 1;
            }
            if (match) break;
        }
        if (!match) continue;
        for (UINT k = 0; k < kTaaInputCount; ++k) taa[k] = srvs[start + k];
        *rootSigOut = rootSig;
        AcquireSRWLockExclusive(&g_taaSeenLock);
        bool seen = false;
        for (UINT i = 0; i < g_taaSeenCount; ++i) seen = seen || g_taaSeenRootSigs[i] == rootSig;
        if (!seen && g_taaSeenCount < ARRAYSIZE(g_taaSeenRootSigs)) g_taaSeenRootSigs[g_taaSeenCount++] = rootSig;
        ReleaseSRWLockExclusive(&g_taaSeenLock);
        if (!seen) logLine("TAA_TABLE matched rootsig=%p param=%u size=%ux%u", rootSig, param, taa[0].width, taa[0].height);
        return true;
    }
    return false;
}

// Called from the game's recording thread on graphics table binds while a dump or constants capture is pending.
void tryRecordTaaDump(ID3D12GraphicsCommandList* list, UINT param, D3D12_GPU_DESCRIPTOR_HANDLE base) {
    DescEntry taa[kTaaInputCount]{};
    void* rootSig = nullptr;
    if (!matchTaaInputs(list, param, base, taa, &rootSig)) return;
    const bool startDump = g_dumpArmed.exchange(false);
    if (startDump) g_cbFramesLeft.store(static_cast<int>(kCbCaptureFrames));
    if (g_cbFramesLeft.load() > 0) captureTaaConstants(list, rootSig, param, base);
    if (!startDump) return;

    ID3D12Device* device = nullptr;
    if (FAILED(list->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) || !device) {
        g_dumpBusy.store(false);
        return;
    }
    if (!g_dumpFence && FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
            reinterpret_cast<void**>(&g_dumpFence)))) {
        device->Release();
        g_dumpBusy.store(false);
        return;
    }

    DumpJob* job = new (std::nothrow) DumpJob;
    if (!job) { device->Release(); g_dumpBusy.store(false); return; }
    job->index = g_dumpIndex.fetch_add(1) + 1;
    job->list = list;
    job->fence = g_dumpFence;
    job->rootSig = rootSig;
    D3D12_HEAP_PROPERTIES readbackHeap{D3D12_HEAP_TYPE_READBACK};
    bool ok = true;
    for (UINT k = 0; k < kTaaInputCount && ok; ++k) {
        auto* resource = static_cast<ID3D12Resource*>(taa[k].resource);
        const D3D12_RESOURCE_DESC desc = resource->GetDesc();
        UINT64 total = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &job->footprint[k], &job->rows[k], &job->rowBytes[k], &total);
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = total; buffer.Height = 1; buffer.DepthOrArraySize = 1; buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ok = SUCCEEDED(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource),
            reinterpret_cast<void**>(&job->readback[k])));
        job->source[k] = resource;
        job->format[k] = taa[k].resFormat;
    }
    device->Release();
    if (!ok) {
        logLine("DUMP error=readback_alloc");
        freeDumpJob(job);
        g_dumpBusy.store(false);
        return;
    }

    D3D12_RESOURCE_BARRIER barriers[kTaaInputCount]{};
    for (UINT k = 0; k < kTaaInputCount; ++k) {
        barriers[k].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[k].Transition.pResource = static_cast<ID3D12Resource*>(job->source[k]);
        barriers[k].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[k].Transition.StateBefore = kTaaInputState;
        barriers[k].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    list->ResourceBarrier(kTaaInputCount, barriers);
    for (UINT k = 0; k < kTaaInputCount; ++k) {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = job->readback[k];
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = job->footprint[k];
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = static_cast<ID3D12Resource*>(job->source[k]);
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    for (D3D12_RESOURCE_BARRIER& b : barriers) {
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = kTaaInputState;
    }
    list->ResourceBarrier(kTaaInputCount, barriers);

    job->recordedTick = GetTickCount64();
    t_psoJob = job;
    g_dumpRecorded.store(job);
    logLine("DUMP index=%u recorded list=%p size=%ux%u srcs=%p,%p,%p,%p,%p,%p,%p", job->index, list,
        job->footprint[0].Footprint.Width, job->footprint[0].Footprint.Height,
        job->source[0], job->source[1], job->source[2], job->source[3], job->source[4], job->source[5], job->source[6]);
}

// Called from ExecuteCommandLists after the original submit.
void onSubmitForDump(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    DumpJob* job = g_dumpRecorded.load();
    if (!job) return;
    for (UINT i = 0; i < count; ++i) {
        if (static_cast<void*>(lists[i]) != job->list) continue;
        if (g_dumpRecorded.exchange(nullptr) != job) return;
        job->fenceValue = ++g_dumpFenceValue;
        if (FAILED(queue->Signal(job->fence, job->fenceValue))) {
            logLine("DUMP index=%u error=signal", job->index);
            freeDumpJob(job);
            g_dumpBusy.store(false);
            return;
        }
        const HANDLE thread = CreateThread(nullptr, 0, &dumpWriterThread, job, 0, nullptr);
        if (thread) CloseHandle(thread);
        else { freeDumpJob(job); g_dumpBusy.store(false); }
        return;
    }
    // The recorded list was never submitted (e.g. reset and re-recorded): give up after a while.
    if (GetTickCount64() - job->recordedTick > kDumpSubmitTimeoutMs && g_dumpRecorded.exchange(nullptr) == job) {
        logLine("DUMP index=%u error=never_submitted", job->index);
        freeDumpJob(job);
        g_dumpBusy.store(false);
    }
}

void pollDumpKey() {
    if (kRelease) return;   // F8 dump: probe build only.
    const bool down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (!down) { g_f8Down.store(false); return; }
    if (g_f8Down.exchange(true)) return;
    bool expected = false;
    if (!g_dumpBusy.compare_exchange_strong(expected, true)) return;  // Previous dump still in flight.
    g_dumpArmed.store(true);
    logLine("DUMP armed");
    MessageBeep(MB_ICONASTERISK);
}

// One line per texture descriptor: "<prefix> <KIND> <ptr> <w>x<h> rf<res fmt> vf<view fmt> vd<view dim> a<array>".
void traceDescriptor(void* list, const char* prefix, const DescEntry& e) {
    if (!e.resource || e.kind == kDescCbv || e.resDim == D3D12_RESOURCE_DIMENSION_BUFFER) return;
    if (e.width < kMinLoggedDescriptorDim || e.height < kMinLoggedDescriptorDim) return;
    traceLine("T%lu L%p %s %s %p %ux%u rf%u vf%u vd%u a%u", GetCurrentThreadId(), list, prefix,
        kDescKindNames[e.kind < 6 ? e.kind : 0], e.resource, e.width, e.height,
        e.resFormat, e.viewFormat, e.viewDim, e.arraySize);
}

void traceTable(void* list, char pipe, UINT param, D3D12_GPU_DESCRIPTOR_HANDLE base) {
    const ListRootSigs* bound = listRootSigs(list, false);
    void* rootSig = bound ? (pipe == 'C' ? bound->compute : bound->graphics) : nullptr;
    std::vector<RsRange> ranges;
    bool known = false;
    if (rootSig) {
        AcquireSRWLockShared(&g_rsLock);
        auto it = g_rootSigs.find(rootSig);
        if (it != g_rootSigs.end() && param < it->second.size() && it->second[param].table) {
            try { ranges = it->second[param].ranges; known = true; } catch (...) {}
        }
        ReleaseSRWLockShared(&g_rsLock);
    }
    if (!known) ranges.push_back({kDescNone, 16, 0});  // Unknown layout: show the first 16 slots.

    AcquireSRWLockShared(&g_heapLock);
    UINT index = 0;
    const HeapRecord* heap = findHeapByGpuLocked(base.ptr, &index);
    traceLine("T%lu L%p TBL %c p=%u rs=%p idx=%u heap=%d layout=%d", GetCurrentThreadId(), list, pipe,
        param, rootSig, index, heap ? 1 : 0, known ? 1 : 0);
    if (heap) {
        char prefix[48];
        UINT logged = 0;
        for (size_t r = 0; r < ranges.size() && logged < kMaxDescriptorsPerTableLog; ++r) {
            const RsRange& range = ranges[r];
            for (UINT j = 0; j < range.count && logged < kMaxDescriptorsPerTableLog; ++j, ++logged) {
                const UINT64 slot = static_cast<UINT64>(index) + range.offset + j;
                if (slot >= heap->count) break;
                snprintf(prefix, sizeof(prefix), "DESC %c p=%u r=%llu i=%u", pipe, param,
                    static_cast<unsigned long long>(r), range.offset + j);
                traceDescriptor(list, prefix, heap->entries[slot]);
            }
        }
    }
    ReleaseSRWLockShared(&g_heapLock);
}

void STDMETHODCALLTYPE HookedDrawInstanced(ID3D12GraphicsCommandList* self, UINT vertices,
    UINT instances, UINT startVertex, UINT startInstance) {
    noteFired(g_firedDraw, "Draw");
    if (capturing()) traceLine("T%lu L%p DRAW %u %u", GetCurrentThreadId(), static_cast<void*>(self), vertices, instances);
    if (!t_inOwnWork && skipTaaDraw(self)) return;
    reinterpret_cast<DrawFn>(findListTable(self)->draw)(self, vertices, instances, startVertex, startInstance);
    if (!t_inOwnWork) afterGameDraw(self);
}
void STDMETHODCALLTYPE HookedDrawIndexedInstanced(ID3D12GraphicsCommandList* self, UINT indices,
    UINT instances, UINT startIndex, INT baseVertex, UINT startInstance) {
    noteFired(g_firedDraw, "Draw");
    if (capturing()) traceLine("T%lu L%p DRAWI %u %u", GetCurrentThreadId(), static_cast<void*>(self), indices, instances);
    if (!t_inOwnWork && skipTaaDraw(self)) return;
    reinterpret_cast<DrawIndexedFn>(findListTable(self)->drawIndexed)(
        self, indices, instances, startIndex, baseVertex, startInstance);
    if (!t_inOwnWork) afterGameDraw(self);
}
void STDMETHODCALLTYPE HookedSetComputeRootSignature(ID3D12GraphicsCommandList* self, ID3D12RootSignature* rs) {
    noteFired(g_firedRootSig, "SetRootSignature");
    ListRootSigs* bound = listRootSigs(self, true);
    if (bound->compute != rs) std::memset(bound->computeTables, 0, sizeof(bound->computeTables));  // Bindings reset.
    bound->compute = rs;
    if (capturing()) traceLine("T%lu L%p RS C %p", GetCurrentThreadId(), static_cast<void*>(self), static_cast<void*>(rs));
    reinterpret_cast<SetRootSigFn>(findListTable(self)->setComputeRs)(self, rs);
}
void STDMETHODCALLTYPE HookedSetGraphicsRootSignature(ID3D12GraphicsCommandList* self, ID3D12RootSignature* rs) {
    noteFired(g_firedRootSig, "SetRootSignature");
    ListRootSigs* bound = listRootSigs(self, true);
    if (bound->graphics != rs) std::memset(bound->graphicsTables, 0, sizeof(bound->graphicsTables));  // Bindings reset.
    bound->graphics = rs;
    if (capturing()) traceLine("T%lu L%p RS G %p", GetCurrentThreadId(), static_cast<void*>(self), static_cast<void*>(rs));
    reinterpret_cast<SetRootSigFn>(findListTable(self)->setGraphicsRs)(self, rs);
}
void STDMETHODCALLTYPE HookedSetComputeRootDescriptorTable(ID3D12GraphicsCommandList* self, UINT param,
    D3D12_GPU_DESCRIPTOR_HANDLE base) {
    noteFired(g_firedTable, "SetRootDescriptorTable");
    if (capturing()) traceTable(self, 'C', param, base);
    if (param < kTrackedTableParams) listRootSigs(self, true)->computeTables[param] = base.ptr;
    reinterpret_cast<SetTableFn>(findListTable(self)->setComputeTable)(self, param, base);
}
void STDMETHODCALLTYPE HookedSetDescriptorHeaps(ID3D12GraphicsCommandList* self, UINT count,
    ID3D12DescriptorHeap* const* heaps) {
    ListRootSigs* bound = listRootSigs(self, true);
    bound->heapCount = 0;
    for (UINT i = 0; heaps && i < count && i < 2; ++i) bound->heaps[bound->heapCount++] = heaps[i];
    reinterpret_cast<SetDescriptorHeapsFn>(findListTable(self)->setDescriptorHeaps)(self, count, heaps);
}
void STDMETHODCALLTYPE HookedSetGraphicsRootDescriptorTable(ID3D12GraphicsCommandList* self, UINT param,
    D3D12_GPU_DESCRIPTOR_HANDLE base) {
    noteFired(g_firedTable, "SetRootDescriptorTable");
    if (capturing()) traceTable(self, 'G', param, base);
    if (param < kTrackedTableParams) listRootSigs(self, true)->graphicsTables[param] = base.ptr;
    if (!t_inOwnWork) {
        if (g_dumpArmed.load(std::memory_order_relaxed) || g_cbFramesLeft.load(std::memory_order_relaxed) > 0)
            tryRecordTaaDump(self, param, base);
        fsrOnTaaBind(self, param, base);
    }
    reinterpret_cast<SetTableFn>(findListTable(self)->setGraphicsTable)(self, param, base);
}
void STDMETHODCALLTYPE HookedOMSetRenderTargets(ID3D12GraphicsCommandList* self, UINT count,
    const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL singleRange, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    noteFired(g_firedRenderTargets, "OMSetRenderTargets");
    listRootSigs(self, true)->rt0Cpu = (rtvs && count) ? rtvs[0].ptr : 0;
    if (capturing()) {
        traceLine("T%lu L%p OM n=%u ds=%d", GetCurrentThreadId(), static_cast<void*>(self), count, dsv ? 1 : 0);
        AcquireSRWLockShared(&g_heapLock);
        for (UINT i = 0; rtvs && i < count && i < 8; ++i) {
            SIZE_T cpu = rtvs[i].ptr;
            if (singleRange) {
                const HeapRecord* h = findHeapByCpuLocked(rtvs[0].ptr);
                cpu = rtvs[0].ptr + static_cast<SIZE_T>(i) * (h ? h->increment : 0);
            }
            if (const DescEntry* e = findEntryByCpuLocked(cpu)) {
                char prefix[16];
                snprintf(prefix, sizeof(prefix), "RT i=%u", i);
                traceDescriptor(self, prefix, *e);
            }
        }
        if (dsv) {
            if (const DescEntry* e = findEntryByCpuLocked(dsv->ptr)) traceDescriptor(self, "DS", *e);
        }
        ReleaseSRWLockShared(&g_heapLock);
    }
    reinterpret_cast<OMSetRenderTargetsFn>(findListTable(self)->omSetRenderTargets)(self, count, rtvs, singleRange, dsv);
}
void STDMETHODCALLTYPE HookedExecuteIndirect(ID3D12GraphicsCommandList* self, ID3D12CommandSignature* signature,
    UINT maxCount, ID3D12Resource* args, UINT64 argsOffset, ID3D12Resource* countBuffer, UINT64 countOffset) {
    noteFired(g_firedIndirect, "ExecuteIndirect");
    if (capturing()) traceLine("T%lu L%p EXECI sig=%p max=%u", GetCurrentThreadId(), static_cast<void*>(self),
        static_cast<void*>(signature), maxCount);
    if (!t_inOwnWork && skipTaaDraw(self)) return;   // The stock TAA draws are ExecuteIndirect in this game.
    reinterpret_cast<ExecuteIndirectFn>(findListTable(self)->executeIndirect)(
        self, signature, maxCount, args, argsOffset, countBuffer, countOffset);
    if (!t_inOwnWork) afterGameDraw(self);
}

bool writeVtableSlot(void** vtbl, size_t slot, void* hook) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_READWRITE, &oldProtect)) return false;
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&vtbl[slot]), hook);
    DWORD ignored = 0;
    VirtualProtect(&vtbl[slot], sizeof(void*), oldProtect, &ignored);
    return true;
}

void registerListTable(void** vt) {
    if (!vt) return;
    AcquireSRWLockExclusive(&g_tableLock);
    const int n = g_listTableCount.load();
    bool known = false;
    for (int i = 0; i < n; ++i) if (g_listTables[i].vtbl == vt) known = true;
    if (!known && n < kMaxTables) {
        ListTable& t = g_listTables[n];
        t.vtbl = vt; t.close = vt[kListClose]; t.reset = vt[kListReset]; t.dispatch = vt[kListDispatch];
        t.copyResource = vt[kListCopyResource]; t.setPso = vt[kListSetPipelineState];
        t.barrier = vt[kListResourceBarrier];
        t.draw = vt[kListDrawInstanced]; t.drawIndexed = vt[kListDrawIndexedInstanced];
        t.setComputeRs = vt[kListSetComputeRootSignature]; t.setGraphicsRs = vt[kListSetGraphicsRootSignature];
        t.setComputeTable = vt[kListSetComputeRootDescriptorTable];
        t.setGraphicsTable = vt[kListSetGraphicsRootDescriptorTable];
        t.omSetRenderTargets = vt[kListOMSetRenderTargets]; t.executeIndirect = vt[kListExecuteIndirect];
        t.setDescriptorHeaps = vt[kListSetDescriptorHeaps];
        g_listTableCount.store(n + 1, std::memory_order_release);
        const bool ok =
            writeVtableSlot(vt, kListClose, reinterpret_cast<void*>(&HookedClose)) &&
            writeVtableSlot(vt, kListReset, reinterpret_cast<void*>(&HookedReset)) &&
            writeVtableSlot(vt, kListDispatch, reinterpret_cast<void*>(&HookedDispatch)) &&
            writeVtableSlot(vt, kListCopyResource, reinterpret_cast<void*>(&HookedCopyResource)) &&
            writeVtableSlot(vt, kListSetPipelineState, reinterpret_cast<void*>(&HookedSetPipelineState)) &&
            writeVtableSlot(vt, kListResourceBarrier, reinterpret_cast<void*>(&HookedResourceBarrier)) &&
            writeVtableSlot(vt, kListDrawInstanced, reinterpret_cast<void*>(&HookedDrawInstanced)) &&
            writeVtableSlot(vt, kListDrawIndexedInstanced, reinterpret_cast<void*>(&HookedDrawIndexedInstanced)) &&
            writeVtableSlot(vt, kListSetComputeRootSignature, reinterpret_cast<void*>(&HookedSetComputeRootSignature)) &&
            writeVtableSlot(vt, kListSetGraphicsRootSignature, reinterpret_cast<void*>(&HookedSetGraphicsRootSignature)) &&
            writeVtableSlot(vt, kListSetComputeRootDescriptorTable, reinterpret_cast<void*>(&HookedSetComputeRootDescriptorTable)) &&
            writeVtableSlot(vt, kListSetGraphicsRootDescriptorTable, reinterpret_cast<void*>(&HookedSetGraphicsRootDescriptorTable)) &&
            writeVtableSlot(vt, kListOMSetRenderTargets, reinterpret_cast<void*>(&HookedOMSetRenderTargets)) &&
            writeVtableSlot(vt, kListExecuteIndirect, reinterpret_cast<void*>(&HookedExecuteIndirect)) &&
            writeVtableSlot(vt, kListSetDescriptorHeaps, reinterpret_cast<void*>(&HookedSetDescriptorHeaps));
        logLine("TRACE_LIST_TABLE vtbl=%p patched=%d", static_cast<void*>(vt), ok ? 1 : 0);
    }
    ReleaseSRWLockExclusive(&g_tableLock);
}

void registerQueueTable(void** vt) {
    if (!vt) return;
    AcquireSRWLockExclusive(&g_tableLock);
    const int n = g_queueTableCount.load();
    bool known = false;
    for (int i = 0; i < n; ++i) if (g_queueTables[i].vtbl == vt) known = true;
    if (!known && n < kMaxTables) {
        QueueTable& t = g_queueTables[n];
        t.vtbl = vt; t.execute = vt[kQueueExecuteCommandLists];
        g_queueTableCount.store(n + 1, std::memory_order_release);
        const bool ok = writeVtableSlot(vt, kQueueExecuteCommandLists,
            reinterpret_cast<void*>(&HookedExecuteCommandLists));
        logLine("TRACE_QUEUE_TABLE vtbl=%p patched=%d", static_cast<void*>(vt), ok ? 1 : 0);
    }
    ReleaseSRWLockExclusive(&g_tableLock);
}

// Create a throwaway allocator/list/queue of each type on the game's device and
// patch the runtime classes they belong to, so every game object of that class
// goes through the trace hooks.
void installTraceHooks(ID3D12Device* device) {
    static std::atomic<ID3D12Device*> s_lastDevice{nullptr};
    if (!device || s_lastDevice.exchange(device) == device) return;
    const D3D12_COMMAND_LIST_TYPE types[3] = {
        D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE, D3D12_COMMAND_LIST_TYPE_COPY };
    for (const D3D12_COMMAND_LIST_TYPE type : types) {
        ID3D12CommandAllocator* allocator = nullptr;
        if (SUCCEEDED(device->CreateCommandAllocator(type, __uuidof(ID3D12CommandAllocator),
                reinterpret_cast<void**>(&allocator))) && allocator) {
            ID3D12GraphicsCommandList* list = nullptr;
            if (SUCCEEDED(device->CreateCommandList(0, type, allocator, nullptr,
                    __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&list))) && list) {
                registerListTable(*reinterpret_cast<void***>(list));
                list->Close();
                list->Release();
            }
            allocator->Release();
        }
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = type;
        ID3D12CommandQueue* queue = nullptr;
        if (SUCCEEDED(device->CreateCommandQueue(&desc, __uuidof(ID3D12CommandQueue),
                reinterpret_cast<void**>(&queue))) && queue) {
            registerQueueTable(*reinterpret_cast<void***>(queue));
            queue->Release();
        }
    }
    logLine("TRACE_HOOKS_INSTALLED list_tables=%d queue_tables=%d",
        g_listTableCount.load(), g_queueTableCount.load());
}


// ============================================================================
// FFX context test. ~8 s after the game's last D3D12CreateDevice, a worker
// thread loads amd_fidelityfx_loader_dx12.dll from the exe folder, enumerates
// the upscaler providers for the game's device, creates (and immediately
// destroys) an upscale context for each provider and for the default, and
// logs FFX_TEST lines. No dispatch, no rendering.
// ============================================================================
constexpr ULONGLONG kFfxTestDelayMs = 8000;
constexpr uint32_t kFfxMaxRenderWidth = 2560;
constexpr uint32_t kFfxMaxRenderHeight = 1440;
constexpr uint32_t kFfxMaxUpscaleWidth = 3840;
constexpr uint32_t kFfxMaxUpscaleHeight = 2160;
constexpr uint64_t kFfxMaxVersions = 16;

struct FfxFns {
    PfnFfxCreateContext createContext = nullptr;
    PfnFfxDestroyContext destroyContext = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxQuery query = nullptr;
};

SRWLOCK g_ffxLock = SRWLOCK_INIT;
ID3D12Device* g_ffxDevice = nullptr;   // AddRef'd; latest device created by the game.
ULONGLONG g_ffxDeviceTick = 0;
bool g_ffxTaken = false;               // Guarded by g_ffxLock.
std::atomic<bool> g_ffxThreadStarted{false};

void ffxMessage(uint32_t type, const wchar_t* message) {
    char text[768]{};
    if (message) WideCharToMultiByte(CP_UTF8, 0, message, -1, text, sizeof(text) - 1, nullptr, nullptr);
    logLine("FFX_TEST msg type=%u %s", type, text);
}

void ffxTryCreate(const FfxFns& fns, ID3D12Device* device, uint64_t versionId, const char* label) {
    ffxCreateContextDescUpscale upscale{};
    upscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    upscale.flags = 0;
    upscale.maxRenderSize = {kFfxMaxRenderWidth, kFfxMaxRenderHeight};
    upscale.maxUpscaleSize = {kFfxMaxUpscaleWidth, kFfxMaxUpscaleHeight};
    upscale.fpMessage = &ffxMessage;

    ffxCreateContextDescUpscaleVersion version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    version.version = FFX_UPSCALER_VERSION;

    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = device;

    ffxOverrideVersion overrideVersion{};
    overrideVersion.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    overrideVersion.versionId = versionId;

    upscale.header.pNext = &version.header;
    version.header.pNext = &backend.header;
    backend.header.pNext = versionId ? &overrideVersion.header : nullptr;

    ffxContext context = nullptr;
    const ULONGLONG start = GetTickCount64();
    const ffxReturnCode_t rc = fns.createContext(&context, &upscale.header, nullptr);
    const ULONGLONG elapsed = GetTickCount64() - start;
    logLine("FFX_TEST create label=%s override_id=0x%llX rc=%u ms=%llu context=%p",
        label, static_cast<unsigned long long>(versionId), rc,
        static_cast<unsigned long long>(elapsed), context);
    if (rc != FFX_API_RETURN_OK || !context) return;

    ffxQueryGetProviderVersion provider{};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    const ffxReturnCode_t queryRc = fns.query(&context, &provider.header);
    logLine("FFX_TEST provider label=%s rc=%u provider_id=0x%llX provider_name=%s",
        label, queryRc, static_cast<unsigned long long>(provider.versionId),
        provider.versionName ? provider.versionName : "(null)");

    const ffxReturnCode_t destroyRc = fns.destroyContext(&context, nullptr);
    logLine("FFX_TEST destroy label=%s rc=%u", label, destroyRc);
}

void ffxRunTest(ID3D12Device* device) {
    logLine("FFX_TEST begin device=%p header_version=0x%X", device, static_cast<unsigned>(FFX_UPSCALER_VERSION));

    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{D3D_SHADER_MODEL_6_6};
    const HRESULT smHr = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,
        &shaderModel, sizeof(shaderModel));
    logLine("FFX_TEST shader_model hr=0x%08X highest=0x%X (FSR 4 needs >= 0x66)",
        static_cast<unsigned>(smHr), static_cast<unsigned>(shaderModel.HighestShaderModel));

    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (length == 0 || length >= ARRAYSIZE(path)) { logLine("FFX_TEST error=exe_path"); return; }
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) { logLine("FFX_TEST error=exe_path"); return; }
    slash[1] = L'\0';
    if (FAILED(StringCchCatW(path, ARRAYSIZE(path), L"amd_fidelityfx_loader_dx12.dll"))) return;

    // Altered search path so the loader finds amd_fidelityfx_upscaler_dx12.dll next to it.
    HMODULE loader = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    logLine("FFX_TEST loader module=%p error=%lu", loader, loader ? 0ul : GetLastError());
    if (!loader) return;

    FfxFns fns;
    fns.createContext = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(loader, "ffxCreateContext"));
    fns.destroyContext = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(loader, "ffxDestroyContext"));
    fns.configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(loader, "ffxConfigure"));
    fns.query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(loader, "ffxQuery"));
    if (!fns.createContext || !fns.destroyContext || !fns.query) {
        logLine("FFX_TEST error=missing_exports create=%p destroy=%p query=%p",
            fns.createContext, fns.destroyContext, fns.query);
        return;
    }

    if (fns.configure) {
        ffxConfigureDescGlobalDebug1 debug{};
        debug.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1;
        debug.fpMessage = &ffxMessage;
        debug.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
        logLine("FFX_TEST global_debug rc=%u", fns.configure(nullptr, &debug.header));
    }

    uint64_t count = 0;
    ffxQueryDescGetVersions versions{};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    versions.device = device;
    versions.outputCount = &count;
    ffxReturnCode_t rc = fns.query(nullptr, &versions.header);
    logLine("FFX_TEST versions_count rc=%u count=%llu", rc, static_cast<unsigned long long>(count));

    uint64_t ids[kFfxMaxVersions]{};
    const char* names[kFfxMaxVersions]{};
    if (rc == FFX_API_RETURN_OK && count > 0) {
        count = count < kFfxMaxVersions ? count : kFfxMaxVersions;
        versions.versionIds = ids;
        versions.versionNames = names;
        rc = fns.query(nullptr, &versions.header);
        logLine("FFX_TEST versions_list rc=%u count=%llu", rc, static_cast<unsigned long long>(count));
        if (rc != FFX_API_RETURN_OK) count = 0;
        for (uint64_t i = 0; i < count; ++i) {
            logLine("FFX_TEST version[%llu] id=0x%llX name=%s", static_cast<unsigned long long>(i),
                static_cast<unsigned long long>(ids[i]), names[i] ? names[i] : "(null)");
        }
    } else {
        count = 0;
    }

    for (uint64_t i = 0; i < count; ++i) {
        char label[32]{};
        snprintf(label, sizeof(label), "version%llu", static_cast<unsigned long long>(i));
        ffxTryCreate(fns, device, ids[i], label);
    }
    ffxTryCreate(fns, device, 0, "default");
    // The loader stays loaded: providers may keep driver-side state until process exit.
}

void ffxRunTestGuarded(ID3D12Device* device) {
    __try {
        ffxRunTest(device);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine("FFX_TEST exception code=0x%08X", static_cast<unsigned>(GetExceptionCode()));
    }
}

DWORD WINAPI ffxTestThread(LPVOID) {
    ID3D12Device* device = nullptr;
    while (!device) {
        Sleep(500);
        AcquireSRWLockExclusive(&g_ffxLock);
        if (g_ffxDevice && GetTickCount64() - g_ffxDeviceTick >= kFfxTestDelayMs) {
            device = g_ffxDevice;
            g_ffxDevice = nullptr;
            g_ffxTaken = true;
        }
        ReleaseSRWLockExclusive(&g_ffxLock);
    }
    ffxRunTestGuarded(device);
    device->Release();
    logLine("FFX_TEST done");
    MessageBeep(MB_ICONEXCLAMATION);
    return 0;
}

// Remember the newest device; the test runs once, 8 s after device creation settles.
void scheduleFfxTest(ID3D12Device* device) {
    if (!device || kRelease) return;
    // Opt-in: only when run_probe_test.ps1 -FfxTest put acvfsr_ffx_test.txt next to the game exe.
    static const bool s_enabled = [] {
        wchar_t path[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
        wchar_t* slash = length ? wcsrchr(path, L'\\') : nullptr;
        if (!slash) return false;
        slash[1] = L'\0';
        return SUCCEEDED(StringCchCatW(path, ARRAYSIZE(path), L"acvfsr_ffx_test.txt")) &&
            GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    }();
    if (!s_enabled) return;
    AcquireSRWLockExclusive(&g_ffxLock);
    if (g_ffxTaken) {
        // The worker already ran (or is running) the test; later devices are ignored.
        ReleaseSRWLockExclusive(&g_ffxLock);
        return;
    }
    device->AddRef();
    ID3D12Device* previous = g_ffxDevice;
    g_ffxDevice = device;
    g_ffxDeviceTick = GetTickCount64();
    ReleaseSRWLockExclusive(&g_ffxLock);
    if (previous) previous->Release();
    logLine("FFX_TEST scheduled device=%p delay_ms=%llu", device,
        static_cast<unsigned long long>(kFfxTestDelayMs));

    if (!g_ffxThreadStarted.exchange(true)) {
        const HANDLE thread = CreateThread(nullptr, 0, &ffxTestThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
        else logLine("FFX_TEST error=create_thread %lu", GetLastError());
    }
}


// ============================================================================
// FSR 4 upscale (prototype). Each frame, on the game's own command list:
//  1. At the stock TAA's table bind: our compute pass copies the jittered color and writes
//     FFX-convention motion vectors (RG16F, current -> previous, UV) and reversed-Z device
//     depth (R32F) into our own textures (the game's are transient/aliased by FSR 1 time).
//     MVs: game t1 decode at the closest-depth texel of the 3x3, or camera reprojection for
//     sky/far pixels, exactly like the TAA shader (see CLAUDE.md "TAA SHADER DECODED").
//  2. Right after FSR 1's RCAS dispatch (UAV and SRV both display-size): ffxDispatch upscales
//     into the RCAS output (already a UAV), overwriting the FSR 1 image.
// After each insertion the game's descriptor heaps, compute root signature + tables, graphics
// tables and pipeline state are restored. F7 toggles FSR 4 (history resets when re-enabled).
// The stock TAA + FSR 1 still run underneath (simple and safe for a first version).
// ============================================================================
constexpr const char kMvPassHlsl[] = R"(
cbuffer C : register(b0) {
    float4 gSize;          // w, h, 1/w, 1/h
    float4 gDepth;         // sky threshold (linear), P22, P23, clip z used for sky reprojection
    float4 gInvProj[4];    // CB0[69..72]
    float4 gCamToWorld[3]; // CB0[6..8]
    float4 gPrevVP[3];     // CB0[38], [39], [41] (rows x, y, w of previous jittered viewProj)
    float4 gJitterFix;     // xy: (previous - current) jitter in UV, cancels jitter in reprojected MVs
};
Texture2D<float4> tColor : register(t0);
Texture2D<float4> tPackedMv : register(t1);
Texture2D<float> tLinDepth : register(t2);
RWTexture2D<float4> uColor : register(u0);
RWTexture2D<float2> uMv : register(u1);
RWTexture2D<float> uDepth : register(u2);
RWTexture2D<float> uExposure : register(u3);   // 1x1, LDR input -> exposure 1

float decodeMv(uint q) {
    float v = clamp(q * 0.000489 - 1.0, -1.0, 1.0);
    float v2 = v * v;
    return sign(v) * 0.2 * v2 * v2;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    int2 size = int2(gSize.xy);
    int2 p = int2(id.xy);
    if (p.x >= size.x || p.y >= size.y) return;
    if (p.x == 0 && p.y == 0) uExposure[int2(0, 0)] = 1.0;
    uColor[p] = tColor[p];
    float d = tLinDepth[p];
    uDepth[p] = saturate(gDepth.z / max(d, 1e-6) - gDepth.y);

    // Per-pixel vectors: unlike the stock TAA we don't dilate (FSR dilates with depth itself).
    float2 uv = (float2(p) + 0.5) * gSize.zw;
    float2 mv;  // game convention: current - previous, UV
    if (d >= gDepth.x) {
        float4 c = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, gDepth.w, 1.0);
        float4 v = float4(dot(c, gInvProj[0]), dot(c, gInvProj[1]), dot(c, gInvProj[2]), dot(c, gInvProj[3]));
        float4 vp = float4(v.xyz / v.w, 1.0);
        float4 w = float4(dot(vp, gCamToWorld[0]), dot(vp, gCamToWorld[1]), dot(vp, gCamToWorld[2]), 1.0);
        float2 prevUv = float2(dot(w, gPrevVP[0]), dot(w, gPrevVP[1])) / dot(w, gPrevVP[2]) * float2(0.5, -0.5) + 0.5;
        mv = uv - prevUv + gJitterFix.xy;
    } else {
        uint3 m = uint3(round(tPackedMv[p].xyz * 255.0));
        mv = float2(decodeMv(m.x * 16 + (m.y >> 4)), decodeMv((m.y & 15) * 256 + m.z));
    }
    uMv[p] = -mv;
}
)";

constexpr const char kIndicatorHlsl[] = R"(
cbuffer I : register(b0) { float4 gColor; uint2 gOrigin; uint2 gSize; };
RWTexture2D<float4> uTarget : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gSize.x || id.y >= gSize.y) return;
    uTarget[gOrigin + id.xy] = gColor;
}
)";
// On-screen text (F7 / F4 / sharpness / mode notifications): 5x7 bitmap font, drawn as a box with a dark background.
// Glyph rows 0-3 in .x, rows 4-6 in .y, 5 bits per row, bit 4 = leftmost pixel. Codes: A-Z = 0-25, 0-9 = 26-35,
// then ' ' . : - + / > (36-42).
constexpr const char kTextHlsl[] = R"(
static const uint2 kFont[43] = {
    uint2(0xFC62E, 0x4631), uint2(0xF463E, 0x7A31), uint2(0x8422E, 0x3A30), uint2(0x8C63E, 0x7A31), uint2(0xF421F, 0x7E10),
    uint2(0xF421F, 0x4210), uint2(0xBC22E, 0x3E31), uint2(0xFC631, 0x4631), uint2(0x2108E, 0x3884), uint2(0x10847, 0x3242),
    uint2(0xC5251, 0x4654), uint2(0x84210, 0x7E10), uint2(0xAD771, 0x4631), uint2(0xAE631, 0x4633), uint2(0x8C62E, 0x3A31),
    uint2(0xF463E, 0x4210), uint2(0x8C62E, 0x3655), uint2(0xF463E, 0x4654), uint2(0x7420F, 0x7821), uint2(0x2109F, 0x1084),
    uint2(0x8C631, 0x3A31), uint2(0x8C631, 0x1151), uint2(0xAC631, 0x2AB5), uint2(0x22A31, 0x462A), uint2(0x22A31, 0x1084),
    uint2(0x2083F, 0x7E08), uint2(0xACE2E, 0x3A39), uint2(0x21184, 0x3884), uint2(0x1062E, 0x7D04), uint2(0x1105F, 0x3A21),
    uint2(0x928C2, 0x85F), uint2(0xFA1F, 0x3A21), uint2(0xF4106, 0x3A31), uint2(0x2083F, 0x2108), uint2(0x7462E, 0x3A31),
    uint2(0x7C62E, 0x3041), uint2(0x0, 0x0), uint2(0x0, 0x3180), uint2(0x3180, 0x18C), uint2(0xF8000, 0x0),
    uint2(0xF9080, 0x84), uint2(0x20841, 0x4108), uint2(0x11110, 0x4104)};
cbuffer T : register(b0) { float4 gTextColor; float4 gBackColor; uint2 gOrigin; uint gScale; uint gCount; uint4 gChars0; uint4 gChars1; };
RWTexture2D<float4> uTarget : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint cellW = 6 * gScale;
    uint w = gCount * cellW + 3 * gScale, h = 10 * gScale;
    if (id.x >= w || id.y >= h) return;
    float4 c = gBackColor;
    int lx = int(id.x) - int(2 * gScale), ly = int(id.y) - int(2 * gScale);
    if (lx >= 0 && ly >= 0) {
        uint ci = uint(lx) / cellW, px = (uint(lx) % cellW) / gScale, py = uint(ly) / gScale;
        if (ci < gCount && px < 5 && py < 7) {
            uint packed = ci < 16 ? gChars0[ci / 4] : gChars1[(ci - 16) / 4];
            uint code = min((packed >> ((ci % 4) * 8)) & 0xFF, 42u);
            uint bits = py < 4 ? kFont[code].x : kFont[code].y;
            uint row = py < 4 ? py : py - 4;
            if ((bits >> (row * 5 + (4 - px))) & 1) c = gTextColor;
        }
    }
    uTarget[gOrigin + id.xy] = c;
}
)";
constexpr UINT kTextConstants = 20;
constexpr UINT kTextMaxChars = 32;
constexpr UINT kTextScale = 4;           // 5x7 font -> 20x28 px glyphs at 4K.
constexpr UINT kTextPosX = 32, kTextPosY = 72;
constexpr ULONGLONG kTextShowMs = 2500;
constexpr UINT kMenuMaxLines = 9;
constexpr UINT kMenuLineHeight = 10 * kTextScale;
constexpr UINT kMenuWidth = 30;          // Characters per menu line (padded).
constexpr UINT kIndicatorSlots = 64;     // Indicator + text descriptors: a few per frame, never reused while in flight.
constexpr UINT kIndicatorPos = 32;     // Top-left corner, pixels.
constexpr UINT kIndicatorSize = 24;
constexpr ULONGLONG kIndicatorShowMs = 3000;

struct MvConstants {
    float size[4];
    float depth[4];
    float invProj[16];
    float camToWorld[12];
    float prevViewProj[12];
    float jitterFix[4];
};
constexpr UINT kMvConstantCount = sizeof(MvConstants) / 4;
static_assert(kMvConstantCount == 52, "root constants must match the cbuffer");
constexpr UINT kOwnTextureCount = 4;          // color, MV, depth, exposure
constexpr UINT kOwnDescriptorsPerFrame = 3 + kOwnTextureCount;   // 3 SRVs (game inputs) + UAVs (our textures)
constexpr UINT kOwnDescriptorFrames = 16;     // Ring depth: never reuse a slot while the GPU may still read it.
constexpr unsigned kFsrMaxLoggedFrames = 5;
constexpr float kSkyThresholdScale = 0.98f;
constexpr unsigned kStableFramesForSkip = 60;
constexpr unsigned kNativeAfterFrames = 60;   // No FSR 1 pass for this many frames -> native mode (game FSR off).
constexpr float kFsrSharpness = 0.3f;
constexpr UINT kTaaDrawsPerFrame = 3;
constexpr unsigned kFsrMaxMessages = 100;

using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
    LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
using SerializeRootSignatureFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION,
    ID3DBlob**, ID3DBlob**);

struct Upscaler {
    bool broken = false;                       // Something failed: stay off for this session.
    ID3D12RootSignature* rootSig = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    UINT increment = 0;
    UINT ring = 0;
    ID3D12RootSignature* indicatorRootSig = nullptr;
    ID3D12PipelineState* indicatorPso = nullptr;
    ID3D12RootSignature* textRootSig = nullptr;
    ID3D12PipelineState* textPso = nullptr;
    char message[kTextMaxChars + 1] = {};     // On-screen notification text (upper case), shown until messageUntil.
    float messageColor[3] = {1.0f, 1.0f, 1.0f};
    ULONGLONG messageUntil = 0;
    ID3D12Resource* overlayScratch = nullptr; // FSR 4 off + native mode: RT0 is copied here, text drawn, copied back.
    UINT overlayScratchW = 0, overlayScratchH = 0;
    DXGI_FORMAT overlayScratchFormat = DXGI_FORMAT_UNKNOWN;
    float sharpness = kFsrSharpness;          // F8 / F9 (release), saved in settings.ini
    UINT jitterPhases = 8;                     // F2 menu: 8 (game default) / 16 / 32, saved in settings.ini
    bool menuOpen = false;                    // F2 overlay menu
    UINT menuSelection = 0;
    UINT indicatorRing = 0;
    ULONGLONG indicatorUntil = 0;              // Show the indicator until this tick (F7 / mode change / start-up).
    bool indicatorAlways = false;              // F3
    ID3D12Resource* color = nullptr;
    ID3D12Resource* mv = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* exposure = nullptr;
    UINT width = 0, height = 0;
    DXGI_FORMAT colorFormat = DXGI_FORMAT_UNKNOWN;
    bool texturesReadable = false;             // false = in UAV state

    HMODULE loader = nullptr;
    PfnFfxCreateContext createContext = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    ffxContext context = nullptr;
    UINT contextRenderW = 0, contextRenderH = 0, contextOutW = 0, contextOutH = 0;
    bool contextHdr = false;

    // Most recent prepared frame (any list): fallback when FSR 1 runs on a different command list than the TAA.
    // The list that recorded the MV pass also keeps its own copy (ListRootSigs::fsrPrep), preferred when RCAS is there.
    PrepData last{};
    bool lastValid = false;
    void* lastList = nullptr;
    unsigned seq = 0;

    float prevJitter[2]{};
    bool havePrevJitter = false;
    LONGLONG lastQpc = 0;
    bool needReset = true;
    unsigned preparedFrames = 0, upscaledFrames = 0, orphanPreps = 0;
    unsigned sameList = 0, otherList = 0, noPrep = 0, rejected = 0, candidates = 0;
    bool skipStock = true;                     // F4: skip the stock TAA draws + FSR 1 dispatches while FSR 4 is on.
    unsigned stableFrames = 0;                 // Consecutive clean same-list frames (skipping needs kStableFramesForSkip).
    unsigned skippedTaa = 0, skippedFsr1 = 0;
    unsigned taaFrames = 0;                    // TAA matches, also counted with FSR 4 off (frame-rate measurement).
    unsigned lastFsr1Frame = 0;                // taaFrames when an FSR 1 pass was last seen (native mode when stale).
    bool nativeMode = false;
    ID3D12Resource* nativeOut = nullptr;       // Native mode FFX output (copied into the TAA render target).
    UINT nativeOutW = 0, nativeOutH = 0;
    DXGI_FORMAT nativeOutFormat = DXGI_FORMAT_UNKNOWN;
    unsigned nativeFrames = 0;
    void* matchedInputs[12]{};                 // Distinct TAA color inputs seen (diagnostics).
    UINT matchedInputCount = 0;
};
Upscaler g_up;
SRWLOCK g_upLock = SRWLOCK_INIT;
std::atomic<bool> g_fsrEnabled{true};
std::atomic<bool> g_f7Down{false};
std::atomic<unsigned> g_fsrMessages{0};
std::atomic<bool> g_fsrDebugView{false};
std::atomic<bool> g_f3Down{false}, g_f4Down{false}, g_f5Down{false}, g_f6Down{false};
std::atomic<bool> g_f8SharpDown{false}, g_f9SharpDown{false};
std::atomic<bool> g_f2Down{false}, g_upDown{false}, g_downDown{false}, g_leftDown{false}, g_rightDown{false}, g_enterDown{false};
void pollJitterScan();
void recordJitter(float x, float y, UINT width, UINT height);

// F6 A/B capture: readback copies recorded around the FFX dispatch, written by a worker once the GPU is done.
struct AbItem {
    ID3D12Resource* readback = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 rowBytes = 0;
    UINT format = 0;
    const char* name = "";
};
struct AbJob {
    unsigned index = 0;
    void* list = nullptr;
    UINT64 fenceValue = 0;
    ULONGLONG recordedTick = 0;
    AbItem items[4];
    UINT count = 0;
    std::string meta;
};
std::atomic<bool> g_abArmed{false};
std::atomic<bool> g_abBusy{false};
std::atomic<AbJob*> g_abRecorded{nullptr};
std::atomic<unsigned> g_abIndex{0};
ID3D12Fence* g_abFence = nullptr;
UINT64 g_abFenceValue = 0;

// Records stateBefore -> COPY_SOURCE, copy into a new readback buffer, back to stateBefore.
void recordReadback(ID3D12GraphicsCommandList* list, AbJob* job, ID3D12Resource* resource, D3D12_RESOURCE_STATES stateBefore,
    const char* name) {
    if (!resource || job->count >= ARRAYSIZE(job->items)) return;
    ID3D12Device* device = nullptr;
    if (FAILED(list->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) || !device) return;
    if (!g_abFence) device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&g_abFence));
    AbItem& item = job->items[job->count];
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    UINT64 total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &item.footprint, &item.rows, &item.rowBytes, &total);
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total; buffer.Height = 1; buffer.DepthOrArraySize = 1; buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&item.readback));
    device->Release();
    if (FAILED(hr) || !g_abFence) { item.readback = nullptr; return; }
    item.format = static_cast<UINT>(desc.Format);
    item.name = name;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, stateBefore, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = item.readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = item.footprint;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = stateBefore;
    list->ResourceBarrier(1, &barrier);
    ++job->count;
}

void fsrMessage(uint32_t type, const wchar_t* message) {
    if (g_fsrMessages.fetch_add(1) >= kFsrMaxMessages) return;
    char text[768]{};
    if (message) WideCharToMultiByte(CP_UTF8, 0, message, -1, text, sizeof(text) - 1, nullptr, nullptr);
    logLine("FSR_MSG type=%u %s", type, text);
}

void fsrFail(const char* what, HRESULT hr) {
    g_up.broken = true;
    g_up.lastValid = false;
    logLine("FSR_ERROR %s hr=0x%08X -> FSR 4 disabled for this session", what, static_cast<unsigned>(hr));
}

// The FidelityFX loader from the game folder (staged by run_probe_test.ps1). Without it FSR 4 stays off,
// before anything is recorded into the game's lists (dump/trace runs don't stage it).
bool ensureFfxLoader() {
    if (g_up.loader) return true;
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    wchar_t* slash = length ? wcsrchr(path, L'\\') : nullptr;
    if (slash) { slash[1] = L'\0'; StringCchCatW(path, ARRAYSIZE(path), L"amd_fidelityfx_loader_dx12.dll"); }
    g_up.loader = slash ? LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) : nullptr;
    if (!g_up.loader) { fsrFail("load amd_fidelityfx_loader_dx12.dll from the game folder", HRESULT_FROM_WIN32(GetLastError())); return false; }
    g_up.createContext = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(g_up.loader, "ffxCreateContext"));
    g_up.query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(g_up.loader, "ffxQuery"));
    g_up.dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(g_up.loader, "ffxDispatch"));
    if (!g_up.createContext || !g_up.query || !g_up.dispatch) { fsrFail("FFX loader exports", E_FAIL); return false; }
    return true;
}

// Compute PSO + root signature + descriptor heap for the MV pass (once).
bool ensureMvPass(ID3D12Device* device) {
    if (g_up.pso) return true;
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = compiler ? reinterpret_cast<D3DCompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    auto serialize = reinterpret_cast<SerializeRootSignatureFn>(lookupExport(16));
    if (!compile || !serialize) { fsrFail("d3dcompiler_47/D3D12SerializeRootSignature missing", E_FAIL); return false; }
    ID3DBlob* shader = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = compile(kMvPassHlsl, sizeof(kMvPassHlsl) - 1, "acv_mv_pass", nullptr, nullptr, "main", "cs_5_0",
        0, 0, &shader, &errors);
    if (FAILED(hr)) {
        logLine("FSR_ERROR shader: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        if (errors) errors->Release();
        fsrFail("compile MV pass", hr);
        return false;
    }
    if (errors) errors->Release();

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kOwnTextureCount, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, kMvConstantCount};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &ranges[0]};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable = {1, &ranges[1]};
    D3D12_ROOT_SIGNATURE_DESC rsDesc{3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob* rsBlob = nullptr;
    hr = serialize(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errors);
    if (errors) errors->Release();
    if (FAILED(hr)) { shader->Release(); fsrFail("serialize root signature", hr); return false; }
    hr = device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
        __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&g_up.rootSig));
    rsBlob->Release();
    if (FAILED(hr)) { shader->Release(); fsrFail("create root signature", hr); return false; }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = g_up.rootSig;
    psoDesc.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    hr = device->CreateComputePipelineState(&psoDesc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&g_up.pso));
    shader->Release();
    if (FAILED(hr)) { fsrFail("create MV pass PSO", hr); return false; }

    // On-screen indicator: a small solid square drawn into FSR 4's output (root constants + one UAV).
    hr = compile(kIndicatorHlsl, sizeof(kIndicatorHlsl) - 1, "acv_indicator", nullptr, nullptr, "main", "cs_5_0", 0, 0, &shader, &errors);
    if (FAILED(hr)) { if (errors) errors->Release(); fsrFail("compile indicator", hr); return false; }
    if (errors) errors->Release();
    D3D12_DESCRIPTOR_RANGE indRange{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER indParams[2]{};
    indParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    indParams[0].Constants = {0, 0, 8};
    indParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    indParams[1].DescriptorTable = {1, &indRange};
    D3D12_ROOT_SIGNATURE_DESC indRsDesc{2, indParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    hr = serialize(&indRsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errors);
    if (errors) errors->Release();
    if (SUCCEEDED(hr)) {
        hr = device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
            __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&g_up.indicatorRootSig));
        rsBlob->Release();
    }
    if (SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC indPso{};
        indPso.pRootSignature = g_up.indicatorRootSig;
        indPso.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
        hr = device->CreateComputePipelineState(&indPso, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&g_up.indicatorPso));
    }
    shader->Release();
    if (FAILED(hr)) { fsrFail("create indicator pipeline", hr); return false; }

    // On-screen text: 20 root constants + one UAV.
    hr = compile(kTextHlsl, sizeof(kTextHlsl) - 1, "acv_text", nullptr, nullptr, "main", "cs_5_0", 0, 0, &shader, &errors);
    if (FAILED(hr)) {
        logLine("FSR_ERROR text shader: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        if (errors) errors->Release();
        fsrFail("compile text overlay", hr);
        return false;
    }
    if (errors) errors->Release();
    D3D12_ROOT_PARAMETER textParams[2]{};
    textParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    textParams[0].Constants = {0, 0, kTextConstants};
    textParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    textParams[1].DescriptorTable = {1, &indRange};
    D3D12_ROOT_SIGNATURE_DESC textRsDesc{2, textParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    hr = serialize(&textRsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errors);
    if (errors) errors->Release();
    if (SUCCEEDED(hr)) {
        hr = device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
            __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&g_up.textRootSig));
        rsBlob->Release();
    }
    if (SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC textPso{};
        textPso.pRootSignature = g_up.textRootSig;
        textPso.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
        hr = device->CreateComputePipelineState(&textPso, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&g_up.textPso));
    }
    shader->Release();
    if (FAILED(hr)) { fsrFail("create text overlay pipeline", hr); return false; }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        kOwnDescriptorsPerFrame * kOwnDescriptorFrames + kIndicatorSlots, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    hr = device->CreateDescriptorHeap(&heapDesc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&g_up.heap));
    if (FAILED(hr)) { fsrFail("create descriptor heap", hr); return false; }
    g_up.increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    logLine("FSR_MV_PASS ready");
    return true;
}

// Shows the indicator for a few seconds after start-up, F7 (on), or a mode change; F3 keeps it on permanently.
void flashIndicator() { g_up.indicatorUntil = GetTickCount64() + kIndicatorShowMs; }

// Shows an on-screen notification (upper-case letters, digits, space and . : - + /) for a few seconds.
void notify(const char* text, float r, float g, float b) {
    size_t i = 0;
    for (; text[i] && i < kTextMaxChars; ++i) g_up.message[i] = text[i];
    g_up.message[i] = '\0';
    g_up.messageColor[0] = r; g_up.messageColor[1] = g; g_up.messageColor[2] = b;
    g_up.messageUntil = GetTickCount64() + kTextShowMs;
    logLine("FSR_NOTIFY %s", g_up.message);
}

bool messageActive() { return g_up.message[0] && GetTickCount64() < g_up.messageUntil; }

UINT glyphCode(char ch) {
    if (ch >= 'A' && ch <= 'Z') return static_cast<UINT>(ch - 'A');
    if (ch >= 'a' && ch <= 'z') return static_cast<UINT>(ch - 'a');
    if (ch >= '0' && ch <= '9') return 26u + static_cast<UINT>(ch - '0');
    switch (ch) {
    case '.': return 37; case ':': return 38; case '-': return 39; case '+': return 40; case '/': return 41; case '>': return 42;
    default: return 36;
    }
}

// Something to draw: the F2 menu (while open) or a notification (for a few seconds).
bool overlayVisible() { return g_up.menuOpen || messageActive(); }

// One text line (box with dark background), padded with spaces to padTo characters so menu lines line up.
// Requires the text root signature, PSO, heap and UAV table to be bound already.
void dispatchTextLine(ID3D12GraphicsCommandList* list, const char* text, const float rgb[3], float boost, UINT x, UINT y, UINT padTo) {
    UINT count = static_cast<UINT>(strlen(text));
    if (count > kTextMaxChars) count = kTextMaxChars;
    const UINT cells = padTo > count ? (padTo < kTextMaxChars ? padTo : kTextMaxChars) : count;
    UINT c[kTextConstants]{};
    const float color[4] = {rgb[0] * boost, rgb[1] * boost, rgb[2] * boost, 1.0f};
    const float back[4] = {0.02f, 0.02f, 0.02f, 1.0f};
    std::memcpy(&c[0], color, sizeof(color));
    std::memcpy(&c[4], back, sizeof(back));
    c[8] = x; c[9] = y; c[10] = kTextScale; c[11] = cells;
    for (UINT i = 0; i < cells; ++i) c[12 + i / 4] |= (i < count ? glyphCode(text[i]) : 36u) << ((i % 4) * 8);
    list->SetComputeRoot32BitConstants(0, kTextConstants, c, 0);
    const UINT boxW = cells * 6 * kTextScale + 3 * kTextScale, boxH = 10 * kTextScale;
    list->Dispatch((boxW + 7) / 8, (boxH + 7) / 8, 1);
}

void buildMenuLines(char lines[][kTextMaxChars + 1], float colors[][3], UINT* count);

// Draws the F2 menu (if open) or the current notification into `target` (UAV state). HDR targets (scene-referred, before
// tone mapping) get brighter text so it survives the game's tone mapper. Same rules as drawIndicator.
void drawText(ID3D12GraphicsCommandList* list, ID3D12Resource* target, bool hdr) {
    if (!g_up.textPso || !overlayVisible()) return;
    const D3D12_RESOURCE_DESC desc = target->GetDesc();
    const UINT neededW = kTextPosX + kTextMaxChars * 6 * kTextScale + 3 * kTextScale;
    const UINT neededH = kTextPosY + (kMenuMaxLines + 1) * kMenuLineHeight;
    if (desc.Width < neededW || desc.Height < neededH) return;   // Too small for the overlay (not a game output).
    ID3D12Device* device = nullptr;
    if (FAILED(list->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) || !device) return;
    const UINT slot = kOwnDescriptorsPerFrame * kOwnDescriptorFrames + (g_up.indicatorRing++ % kIndicatorSlots);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_up.heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_up.heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(slot) * g_up.increment;
    gpu.ptr += static_cast<UINT64>(slot) * g_up.increment;
    device->CreateUnorderedAccessView(target, nullptr, nullptr, cpu);
    device->Release();

    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = target;
    list->ResourceBarrier(1, &uav);
    ID3D12DescriptorHeap* heaps[1] = {g_up.heap};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_up.textRootSig);
    list->SetComputeRootDescriptorTable(1, gpu);
    list->SetPipelineState(g_up.textPso);
    const float boost = hdr ? 3.0f : 1.0f;
    if (g_up.menuOpen) {
        char lines[kMenuMaxLines][kTextMaxChars + 1]{};
        float colors[kMenuMaxLines][3]{};
        UINT count = 0;
        buildMenuLines(lines, colors, &count);
        for (UINT i = 0; i < count; ++i) {
            dispatchTextLine(list, lines[i], colors[i], boost, kTextPosX, kTextPosY + i * kMenuLineHeight, kMenuWidth);
        }
    } else {
        dispatchTextLine(list, g_up.message, g_up.messageColor, boost, kTextPosX, kTextPosY, 0);
    }
    list->ResourceBarrier(1, &uav);
}

// Draws the indicator into `target` (in UAV state, just written by FFX). Green = FSR 4 upscaling, cyan = FSR 4 native AA.
// Must be called inside t_inOwnWork with the game state saved (it sets our heap / root sig / PSO).
void drawIndicator(ID3D12GraphicsCommandList* list, ID3D12Resource* target, bool native) {
    if (!g_up.indicatorPso || !(g_up.indicatorAlways || GetTickCount64() < g_up.indicatorUntil)) return;
    ID3D12Device* device = nullptr;
    if (FAILED(list->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) || !device) return;
    const UINT slot = kOwnDescriptorsPerFrame * kOwnDescriptorFrames + (g_up.indicatorRing++ % kIndicatorSlots);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_up.heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_up.heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(slot) * g_up.increment;
    gpu.ptr += static_cast<UINT64>(slot) * g_up.increment;
    device->CreateUnorderedAccessView(target, nullptr, nullptr, cpu);
    device->Release();
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = target;
    list->ResourceBarrier(1, &uav);   // FFX wrote it; we write next.
    const float color[4] = {0.0f, 1.0f, native ? 1.0f : 0.0f, 1.0f};
    UINT constants[8];
    std::memcpy(constants, color, sizeof(color));
    constants[4] = kIndicatorPos; constants[5] = kIndicatorPos;
    constants[6] = kIndicatorSize; constants[7] = kIndicatorSize;
    ID3D12DescriptorHeap* heaps[1] = {g_up.heap};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_up.indicatorRootSig);
    list->SetComputeRoot32BitConstants(0, 8, constants, 0);
    list->SetComputeRootDescriptorTable(1, gpu);
    list->SetPipelineState(g_up.indicatorPso);
    list->Dispatch((kIndicatorSize + 7) / 8, (kIndicatorSize + 7) / 8, 1);
    list->ResourceBarrier(1, &uav);
}

ID3D12Resource* createOwnTexture(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format) {
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width; desc.Height = height; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = format; desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* resource = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&resource)))) return nullptr;
    return resource;
}

// Our render-resolution textures; the colour copy uses the game's TAA colour format (RGBA8 tone-mapped or RGBA16F HDR).
// On a size/format change the old ones are leaked on purpose (the GPU may still use them).
bool ensureOwnTextures(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT colorFormat) {
    if (g_up.color && g_up.width == width && g_up.height == height && g_up.colorFormat == colorFormat) return true;
    g_up.color = createOwnTexture(device, width, height, colorFormat);
    g_up.colorFormat = colorFormat;
    g_up.mv = createOwnTexture(device, width, height, DXGI_FORMAT_R16G16_FLOAT);
    g_up.depth = createOwnTexture(device, width, height, DXGI_FORMAT_R32_FLOAT);
    g_up.exposure = createOwnTexture(device, 1, 1, DXGI_FORMAT_R32_FLOAT);
    if (!g_up.color || !g_up.mv || !g_up.depth || !g_up.exposure) { fsrFail("create own textures", E_OUTOFMEMORY); return false; }
    g_up.width = width;
    g_up.height = height;
    g_up.texturesReadable = false;
    g_up.needReset = true;
    logLine("FSR_TEXTURES %ux%u colour=%s", width, height, colorFormat == DXGI_FORMAT_R16G16B16A16_FLOAT ? "HDR RGBA16F" : "RGBA8");
    return true;
}

void transitionOwnTextures(ID3D12GraphicsCommandList* list, bool toReadable) {
    if (g_up.texturesReadable == toReadable) return;
    ID3D12Resource* textures[kOwnTextureCount] = {g_up.color, g_up.mv, g_up.depth, g_up.exposure};
    D3D12_RESOURCE_BARRIER barriers[kOwnTextureCount]{};
    for (UINT i = 0; i < kOwnTextureCount; ++i) {
        barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[i].Transition.pResource = textures[i];
        barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[i].Transition.StateBefore = toReadable ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[i].Transition.StateAfter = toReadable ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    list->ResourceBarrier(kOwnTextureCount, barriers);
    g_up.texturesReadable = toReadable;
}

// Puts back what our passes changed on the game's list: descriptor heaps, compute root signature + tables,
// graphics tables (the graphics root signature itself is never changed by us) and the pipeline state.
void restoreGameState(ID3D12GraphicsCommandList* list, const ListRootSigs& saved) {
    list->SetDescriptorHeaps(saved.heapCount, saved.heaps);
    if (saved.compute) {
        list->SetComputeRootSignature(static_cast<ID3D12RootSignature*>(saved.compute));
        for (UINT p = 0; p < kTrackedTableParams; ++p)
            if (saved.computeTables[p]) list->SetComputeRootDescriptorTable(p, {saved.computeTables[p]});
    }
    if (saved.graphics) {
        for (UINT p = 0; p < kTrackedTableParams; ++p)
            if (saved.graphicsTables[p]) list->SetGraphicsRootDescriptorTable(p, {saved.graphicsTables[p]});
    }
    if (saved.pso) list->SetPipelineState(saved.pso);
    if (ListRootSigs* bound = listRootSigs(list, false)) *bound = saved;
}

void createGameSrv(ID3D12Device* device, const DescEntry& e, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = static_cast<DXGI_FORMAT>(e.viewFormat);
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(static_cast<ID3D12Resource*>(e.resource), &srv, dest);
}

// Step 1: at the TAA table bind.
void fsrOnTaaBind(ID3D12GraphicsCommandList* list, UINT param, D3D12_GPU_DESCRIPTOR_HANDLE base) {
    if (g_up.broken) return;
    DescEntry taa[kTaaInputCount]{};
    void* rootSig = nullptr;
    if (!matchTaaInputs(list, param, base, taa, &rootSig)) return;
    ++g_up.taaFrames;   // Counted with FSR 4 off too: frame rate for the stock / FSR 4 / FSR 4 + skip comparison.
    if (!g_fsrEnabled.load(std::memory_order_relaxed)) {
        // FSR 4 off in native mode: remember the stock TAA so a pending notification is drawn after its last draw.
        if (overlayVisible() && g_up.nativeMode) {
            if (ListRootSigs* mine = listRootSigs(list, false)) {
                mine->overlayPending = true;
                mine->overlayDraws = 0;
                mine->taaTable = base.ptr;
                mine->taaParam = param;
            }
        }
        return;
    }
    const ListRootSigs* bound = listRootSigs(list, false);
    if (!bound || bound->heapCount == 0) return;   // Can't restore the game's heaps: don't touch the list.

    // Per-frame constants straight from the game's constant buffers (CPU copies, see captureTaaConstants).
    uint8_t cb1Bytes[256]{}, cb0Bytes[73 * 16]{};
    UINT64 va = 0;
    if (readTableCbv(rootSig, param, base.ptr, cb1Bytes, sizeof(cb1Bytes), &va) < 7 * 16 ||
        readTableCbv(rootSig, kTaaViewConstantsParam, bound->graphicsTables[kTaaViewConstantsParam], cb0Bytes,
            sizeof(cb0Bytes), &va) < sizeof(cb0Bytes)) {
        static std::atomic<bool> s_logged{false};
        if (!s_logged.exchange(true)) logLine("FSR_WARN constant buffers not readable; skipping frames");
        return;
    }
    const auto* cb1 = reinterpret_cast<const float(*)[4]>(cb1Bytes);
    const auto* cb0 = reinterpret_cast<const float(*)[4]>(cb0Bytes);
    const uint32_t historyValid = reinterpret_cast<const uint32_t*>(cb1Bytes)[0];

    AcquireSRWLockExclusive(&g_upLock);
    ID3D12Device* device = nullptr;
    if (FAILED(list->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) || !device) {
        ReleaseSRWLockExclusive(&g_upLock);
        return;
    }
    const UINT width = taa[0].width, height = taa[0].height;
    const bool hdr = taa[0].resFormat == DXGI_FORMAT_R16G16B16A16_FLOAT;   // Game FSR off: TAA runs on HDR colour.
    if (!ensureFfxLoader() || !ensureMvPass(device) ||
        !ensureOwnTextures(device, width, height, hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM)) {
        device->Release();
        ReleaseSRWLockExclusive(&g_upLock);
        return;
    }

    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = g_up.lastQpc ? static_cast<float>(1000.0 * (now.QuadPart - g_up.lastQpc) / freq.QuadPart) : 16.6f;
    g_up.lastQpc = now.QuadPart;
    if (dt <= 0.0f || dt > 100.0f) dt = 16.6f;

    const float jitterX = cb1[3][0], jitterY = cb1[3][1];
    MvConstants c{};
    c.size[0] = static_cast<float>(width); c.size[1] = static_cast<float>(height);
    c.size[2] = 1.0f / width; c.size[3] = 1.0f / height;
    const float threshold = (cb1[6][0] - cb1[6][1]) / (cb1[6][2] - cb1[6][3]);
    // Sky depth sits at the far plane, but this float threshold lands a hair above it (~32270 vs 32264), so `>=` missed
    // the sky and decoded its stale t1 data as huge motion. Anything within 2% of the far plane counts as sky.
    c.depth[0] = threshold * kSkyThresholdScale; c.depth[1] = cb0[28][2]; c.depth[2] = cb0[28][3]; c.depth[3] = cb0[54][1];
    std::memcpy(c.invProj, cb0[69], sizeof(c.invProj));
    std::memcpy(c.camToWorld, cb0[6], sizeof(c.camToWorld));
    std::memcpy(c.prevViewProj + 0, cb0[38], 16);
    std::memcpy(c.prevViewProj + 4, cb0[39], 16);
    std::memcpy(c.prevViewProj + 8, cb0[41], 16);
    if (g_up.havePrevJitter) {
        c.jitterFix[0] = (g_up.prevJitter[0] - jitterX) / width;
        c.jitterFix[1] = (g_up.prevJitter[1] - jitterY) / height;
    }

    // Descriptors for this frame: game SRVs (color, packed MV, linear depth) + our UAVs.
    const UINT slot = (g_up.ring++ % kOwnDescriptorFrames) * kOwnDescriptorsPerFrame;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_up.heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_up.heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(slot) * g_up.increment;
    gpu.ptr += static_cast<UINT64>(slot) * g_up.increment;
    for (UINT k = 0; k < 3; ++k) createGameSrv(device, taa[k], {cpu.ptr + static_cast<SIZE_T>(k) * g_up.increment});
    ID3D12Resource* own[kOwnTextureCount] = {g_up.color, g_up.mv, g_up.depth, g_up.exposure};
    for (UINT k = 0; k < kOwnTextureCount; ++k) {
        device->CreateUnorderedAccessView(own[k], nullptr, nullptr, {cpu.ptr + static_cast<SIZE_T>(3 + k) * g_up.increment});
    }
    device->Release();

    const ListRootSigs saved = *bound;
    t_inOwnWork = true;
    transitionOwnTextures(list, false);
    ID3D12DescriptorHeap* heaps[1] = {g_up.heap};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_up.rootSig);
    list->SetComputeRoot32BitConstants(0, kMvConstantCount, &c, 0);
    list->SetComputeRootDescriptorTable(1, gpu);
    list->SetComputeRootDescriptorTable(2, {gpu.ptr + 3ull * g_up.increment});
    list->SetPipelineState(g_up.pso);
    list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    transitionOwnTextures(list, true);
    restoreGameState(list, saved);
    t_inOwnWork = false;

    PrepData prep{};
    prep.jitter[0] = jitterX;
    prep.jitter[1] = jitterY;
    prep.nearPlane = cb0[28][3] / (1.0f + cb0[28][2]);
    prep.farPlane = cb0[28][2] > 0.0f ? cb0[28][3] / cb0[28][2] : 100000.0f;
    prep.fovY = 2.0f * atanf(1.0f / cb0[27][1]);
    prep.frameTimeMs = dt;
    prep.reset = g_up.needReset || historyValid == 0;
    prep.hdr = hdr;
    prep.qpc = now.QuadPart;
    prep.seq = ++g_up.seq;
    g_up.needReset = false;
    g_up.prevJitter[0] = jitterX;
    g_up.prevJitter[1] = jitterY;
    g_up.havePrevJitter = true;
    recordJitter(jitterX, jitterY, width, height);

    if (g_up.lastValid) {
        if (++g_up.orphanPreps <= kFsrMaxLoggedFrames) {
            logLine("FSR_ORPHAN_PREP count=%u previous_list=%p this_list=%p (previous prepared frame was never upscaled)",
                g_up.orphanPreps, g_up.lastList, static_cast<void*>(list));
        }
        if (g_up.stableFrames >= kStableFramesForSkip) logLine("FSR_SKIP_OFF reason=orphan_prep");
        g_up.stableFrames = 0;   // Pairing not clean: stop skipping stock work until it is again.
    }
    g_up.last = prep;
    g_up.lastValid = true;
    g_up.lastList = list;
    if (ListRootSigs* mine = listRootSigs(list, false)) {
        mine->fsrPending = true;
        mine->fsrPrep = prep;
        mine->taaTable = base.ptr;
        mine->taaParam = param;
        mine->taaDrawsSkipped = 0;
        mine->easuSkipped = false;
        // Native mode = the game has stopped running FSR 1 (FSR off in its settings): FSR 4 replaces the TAA draws.
        const bool native = g_up.taaFrames > kNativeAfterFrames && g_up.taaFrames - g_up.lastFsr1Frame > kNativeAfterFrames;
        if (native != g_up.nativeMode) {
            g_up.nativeMode = native;
            g_up.stableFrames = 0;     // Upscale-mode skipping re-qualifies after a mode change.
            mine->fsrPrep.reset = true;   // New context / different output: start FSR history fresh this frame.
            g_up.last.reset = true;
            flashIndicator();
            notify(native ? "FSR 4 NATIVE AA MODE" : "FSR 4 UPSCALE MODE", 0.3f, 0.9f, 1.0f);
            logLine("FSR_MODE %s (render %ux%u)", native ? "native (game FSR off: FSR 4 native AA replaces the TAA)"
                : "upscale (game FSR on: FSR 4 replaces FSR 1)", width, height);
        }
        mine->nativeFrame = native;
        mine->nativeDone = false;
    }

    // Diagnostics: which distinct TAA inputs get matched (same table bound repeatedly, or different passes?).
    bool known = false;
    for (UINT i = 0; i < g_up.matchedInputCount; ++i) known = known || g_up.matchedInputs[i] == taa[0].resource;
    if (!known && g_up.matchedInputCount < ARRAYSIZE(g_up.matchedInputs)) {
        g_up.matchedInputs[g_up.matchedInputCount++] = taa[0].resource;
        logLine("FSR_MATCH_INPUTS #%u list=%p rootsig=%p param=%u t0=%p t1=%p t2=%p size=%ux%u prepared=%u",
            g_up.matchedInputCount, static_cast<void*>(list), rootSig, param, taa[0].resource, taa[1].resource,
            taa[2].resource, width, height, g_up.preparedFrames + 1);
    }
    if (g_up.preparedFrames++ < kFsrMaxLoggedFrames) {
        logLine("FSR_PREP frame=%u list=%p size=%ux%u jitter=(%.4f,%.4f) near=%.4f far=%.1f fov=%.4f sky=%.1f dt=%.2f reset=%d",
            g_up.preparedFrames, static_cast<void*>(list), width, height, jitterX, jitterY, prep.nearPlane, prep.farPlane,
            prep.fovY, threshold, dt, prep.reset ? 1 : 0);
    }
    ReleaseSRWLockExclusive(&g_upLock);
}

// Texture entries (SRV or UAV) of a bound table, from its root signature layout.
UINT tableTextures(void* rootSig, UINT param, UINT64 base, uint8_t kind, DescEntry* out, UINT max) {
    UINT n = 0;
    if (!rootSig || !base) return 0;
    AcquireSRWLockShared(&g_rsLock);
    auto it = g_rootSigs.find(rootSig);
    if (it != g_rootSigs.end() && param < it->second.size()) {
        AcquireSRWLockShared(&g_heapLock);
        UINT index = 0;
        const HeapRecord* heap = findHeapByGpuLocked(base, &index);
        for (const RsRange& range : it->second[param].ranges) {
            if (!heap || range.kind != kind) continue;
            for (UINT j = 0; j < range.count && j < 16 && n < max; ++j) {
                const UINT64 slot = static_cast<UINT64>(index) + range.offset + j;
                if (slot >= heap->count) break;
                const DescEntry& e = heap->entries[slot];
                if (e.kind == kind && e.resource && e.resDim == D3D12_RESOURCE_DIMENSION_TEXTURE2D) out[n++] = e;
            }
        }
        ReleaseSRWLockShared(&g_heapLock);
    }
    ReleaseSRWLockShared(&g_rsLock);
    return n;
}

bool ensureFfxContext(ID3D12Device* device, UINT outW, UINT outH, bool hdr) {
    if (g_up.context && g_up.contextOutW == outW && g_up.contextOutH == outH && g_up.contextHdr == hdr &&
        g_up.contextRenderW >= g_up.width && g_up.contextRenderH >= g_up.height) return true;

    // New size / colour space: make a new context. The old one is leaked on purpose (the GPU may still be using it).
    // Tone-mapped 8-bit input: non-linear colour space, exposure 1 via our 1x1 texture.
    // HDR input (game FSR off): linear HDR, FSR computes exposure itself.
    ffxCreateContextDescUpscale upscale{};
    upscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    upscale.flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED |
        (hdr ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE : FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE) |
        (kRelease ? 0u : FFX_UPSCALE_ENABLE_DEBUG_CHECKING | FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION);   // probe: F5 debug view
    upscale.maxRenderSize = {g_up.width, g_up.height};
    upscale.maxUpscaleSize = {outW, outH};
    upscale.fpMessage = &fsrMessage;
    ffxCreateContextDescUpscaleVersion version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    version.version = FFX_UPSCALER_VERSION;
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = device;
    upscale.header.pNext = &version.header;
    version.header.pNext = &backend.header;

    // Optional provider override: ACVFSR_UPSCALER=<version name prefix>, e.g. "3.1.5" (default: the loader's
    // choice, which is the newest = FSR 4 on this system).
    ffxOverrideVersion overrideVersion{};
    overrideVersion.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    char wanted[32]{};
    if (GetEnvironmentVariableA("ACVFSR_UPSCALER", wanted, sizeof(wanted)) > 0) {
        uint64_t count = 0;
        ffxQueryDescGetVersions versions{};
        versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        versions.device = device;
        versions.outputCount = &count;
        uint64_t ids[16]{};
        const char* names[16]{};
        if (g_up.query(nullptr, &versions.header) == FFX_API_RETURN_OK && count > 0) {
            count = count < 16 ? count : 16;
            versions.versionIds = ids;
            versions.versionNames = names;
            g_up.query(nullptr, &versions.header);
            for (uint64_t i = 0; i < count; ++i) {
                if (names[i] && strncmp(names[i], wanted, strlen(wanted)) == 0) {
                    overrideVersion.versionId = ids[i];
                    backend.header.pNext = &overrideVersion.header;
                    logLine("FSR_OVERRIDE %s -> %s", wanted, names[i]);
                    break;
                }
            }
        }
    }

    ffxContext context = nullptr;
    const ffxReturnCode_t rc = g_up.createContext(&context, &upscale.header, nullptr);
    if (rc != FFX_API_RETURN_OK || !context) { fsrFail("ffxCreateContext", static_cast<HRESULT>(rc)); return false; }
    ffxQueryGetProviderVersion provider{};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    g_up.query(&context, &provider.header);
    g_up.context = context;
    g_up.contextRenderW = g_up.width; g_up.contextRenderH = g_up.height;
    g_up.contextOutW = outW; g_up.contextOutH = outH;
    g_up.contextHdr = hdr;
    flashIndicator();   // FSR 4 just started (or changed mode/size): show it.
    g_up.needReset = true;
    logLine("FSR_CONTEXT render=%ux%u output=%ux%u provider=%s (0x%llX)", g_up.width, g_up.height, outW, outH,
        provider.versionName ? provider.versionName : "?", static_cast<unsigned long long>(provider.versionId));
    return true;
}

constexpr double kCrossListPrepMaxAgeMs = 50.0;
constexpr UINT kMaxScannedPerRange = 4096;   // Bound on unbounded/huge ranges.

enum class Fsr1Pass { None, Easu, Rcas };

// Classifies the dispatch about to run / just run on this list from its bound compute tables:
// RCAS = display-size UAV + display-size SRV (output in *output), EASU = display-size UAV only, else None.
Fsr1Pass classifyFsr1Dispatch(ID3D12GraphicsCommandList* list, ListRootSigs* bound, DescEntry* output) {
    // Only display-size (> render size) textures matter, so scan every bound entry of every table without a cap
    // (the game's compute root signatures also carry big material tables that would crowd out a capped list).
    DescEntry uavs[8]{}, srvs[16]{};
    UINT uavCount = 0, srvCount = 0, scanned = 0;
    AcquireSRWLockShared(&g_rsLock);
    auto rsIt = g_rootSigs.find(bound->compute);
    if (rsIt != g_rootSigs.end()) {
        AcquireSRWLockShared(&g_heapLock);
        for (UINT p = 0; p < kTrackedTableParams && p < rsIt->second.size(); ++p) {
            UINT index = 0;
            const HeapRecord* heap = bound->computeTables[p] ? findHeapByGpuLocked(bound->computeTables[p], &index) : nullptr;
            if (!heap) continue;
            for (const RsRange& range : rsIt->second[p].ranges) {
                if (range.kind != kDescSrv && range.kind != kDescUav) continue;
                const UINT count = range.count < kMaxScannedPerRange ? range.count : kMaxScannedPerRange;
                for (UINT j = 0; j < count; ++j) {
                    const UINT64 slot = static_cast<UINT64>(index) + range.offset + j;
                    if (slot >= heap->count) break;
                    ++scanned;
                    const DescEntry& e = heap->entries[slot];
                    if (e.kind != range.kind || !e.resource || e.resDim != D3D12_RESOURCE_DIMENSION_TEXTURE2D) continue;
                    if (e.width <= g_up.width || e.height <= g_up.height) continue;
                    if (e.kind == kDescUav && uavCount < ARRAYSIZE(uavs)) uavs[uavCount++] = e;
                    if (e.kind == kDescSrv && srvCount < ARRAYSIZE(srvs)) srvs[srvCount++] = e;
                }
            }
        }
        ReleaseSRWLockShared(&g_heapLock);
    }
    ReleaseSRWLockShared(&g_rsLock);
    if (uavCount == 0) return Fsr1Pass::None;   // Not an FSR 1 pass (no display-size output).
    ++g_up.candidates;

    for (UINT u = 0; u < uavCount; ++u) {
        for (UINT s = 0; s < srvCount; ++s) {
            if (srvs[s].resource != uavs[u].resource && srvs[s].width == uavs[u].width && srvs[s].height == uavs[u].height) {
                *output = uavs[u];
                return Fsr1Pass::Rcas;
            }
        }
    }
    // Display-size UAV without a display-size SRV = EASU (expected once per frame). Logged sparsely.
    const unsigned rejected = ++g_up.rejected;
    if (rejected <= 4 || rejected % 600 == 0) {
        logLine("FSR_RCAS_REJECT #%u list=%p uav=%ux%u f%u big_srvs=%u scanned=%u pending_here=%d", rejected,
            static_cast<void*>(list), uavs[0].width, uavs[0].height, uavs[0].resFormat, srvCount, scanned,
            bound->fsrPending ? 1 : 0);
    }
    *output = uavs[0];
    return Fsr1Pass::Easu;
}

// Skipping the stock TAA + FSR 1 work is only done once TAA -> RCAS pairing has been clean on the same list for a
// while, never on a frame that an F6 capture needs (it compares against FSR 1), and only for a list with a prepared frame.
bool stockSkipActive(const ListRootSigs* bound) {
    return bound && bound->fsrPending && g_up.skipStock && g_up.stableFrames >= kStableFramesForSkip &&
        g_fsrEnabled.load(std::memory_order_relaxed) && !g_up.broken && !g_abArmed.load(std::memory_order_relaxed);
}

void upscaleAtRcas(ID3D12GraphicsCommandList* list, ListRootSigs* bound, const DescEntry& outputEntry, bool rcasSkipped);

// FSR 1 passes cover the display in 16x16 tiles (240x135 groups for 3840x2160), i.e. more than the render size.
// Cheap pre-filter so only those few dispatches get their bindings scanned.
bool mayBeFsr1Dispatch(UINT x, UINT y) {
    return g_up.width && x * 16u > g_up.width && y * 16u > g_up.height;
}

Fsr1Pass classifyAndNote(ID3D12GraphicsCommandList* list, ListRootSigs* bound, DescEntry* output) {
    const Fsr1Pass pass = classifyFsr1Dispatch(list, bound, output);
    if (pass != Fsr1Pass::None) g_up.lastFsr1Frame = g_up.taaFrames;   // The game runs FSR 1: upscale mode, not native.
    return pass;
}

// Before each game dispatch: when skipping is active, drop EASU, and replace RCAS by the FSR 4 dispatch. Returns true
// when the game's dispatch must not run.
bool beforeGameDispatch(ID3D12GraphicsCommandList* list, UINT x, UINT y) {
    if (!mayBeFsr1Dispatch(x, y)) return false;
    ListRootSigs* bound = listRootSigs(list, false);
    if (!stockSkipActive(bound) || !bound->compute || bound->heapCount == 0 || !g_up.color) return false;
    DescEntry output{};
    switch (classifyAndNote(list, bound, &output)) {
    case Fsr1Pass::Easu:
        if (bound->easuSkipped) return false;
        bound->easuSkipped = true;
        ++g_up.skippedFsr1;
        return true;
    case Fsr1Pass::Rcas:
        upscaleAtRcas(list, bound, output, true);
        ++g_up.skippedFsr1;
        return true;
    default:
        return false;
    }
}

bool nativeUpscale(ID3D12GraphicsCommandList* list, ListRootSigs* bound);

// Stock TAA draws (up to 3 after the prep, with the TAA table still bound). Upscale mode: dropped while skipping is
// active. Native mode (no FSR 1 in the game): FSR 4 replaces them; if that fails the stock TAA simply runs.
bool skipTaaDraw(ID3D12GraphicsCommandList* list) {
    ListRootSigs* bound = listRootSigs(list, false);
    if (!bound || !(bound->fsrPending || bound->nativeDone) || bound->taaDrawsSkipped >= kTaaDrawsPerFrame) return false;
    if (bound->taaParam >= kTrackedTableParams || bound->graphicsTables[bound->taaParam] != bound->taaTable) return false;
    if (bound->nativeFrame) {
        if (!bound->nativeDone && !nativeUpscale(list, bound)) return false;
    } else if (!stockSkipActive(bound)) {
        return false;
    }
    ++bound->taaDrawsSkipped;
    ++g_up.skippedTaa;
    return true;
}

// FSR 4 off, game FSR on: draw the notification (e.g. "FSR 4 OFF") into the stock FSR 1 output right after its RCAS.
void overlayStockRcas(ID3D12GraphicsCommandList* list, ListRootSigs* bound) {
    DescEntry output{};
    if (classifyFsr1Dispatch(list, bound, &output) != Fsr1Pass::Rcas) return;
    AcquireSRWLockExclusive(&g_upLock);
    const ListRootSigs saved = *bound;
    t_inOwnWork = true;
    drawText(list, static_cast<ID3D12Resource*>(output.resource), false);
    restoreGameState(list, saved);
    t_inOwnWork = false;
    ReleaseSRWLockExclusive(&g_upLock);
}

// FSR 4 off, game FSR off: after the last stock TAA draw, copy the TAA output (render target, no UAV access) to a
// scratch texture, draw the notification there and copy it back.
void overlayViaCopy(ID3D12GraphicsCommandList* list, ListRootSigs* bound) {
    DescEntry rt0{};
    AcquireSRWLockShared(&g_heapLock);
    if (const DescEntry* e = findEntryByCpuLocked(bound->rt0Cpu)) rt0 = *e;
    ReleaseSRWLockShared(&g_heapLock);
    if (rt0.kind != kDescRtv || !rt0.resource || rt0.arraySize != 1 || !g_up.textPso) return;
    auto* target = static_cast<ID3D12Resource*>(rt0.resource);
    const auto format = static_cast<DXGI_FORMAT>(rt0.resFormat);
    AcquireSRWLockExclusive(&g_upLock);
    if (!g_up.overlayScratch || g_up.overlayScratchW != rt0.width || g_up.overlayScratchH != rt0.height ||
        g_up.overlayScratchFormat != format) {
        ID3D12Device* device = nullptr;
        if (SUCCEEDED(list->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) && device) {
            g_up.overlayScratch = createOwnTexture(device, rt0.width, rt0.height, format);   // old one leaked on purpose
            device->Release();
        }
        g_up.overlayScratchW = rt0.width; g_up.overlayScratchH = rt0.height; g_up.overlayScratchFormat = format;
    }
    if (!g_up.overlayScratch) { ReleaseSRWLockExclusive(&g_upLock); return; }
    ID3D12Resource* scratch = g_up.overlayScratch;
    auto transition = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        return b;
    };
    const ListRootSigs saved = *bound;
    t_inOwnWork = true;
    D3D12_RESOURCE_BARRIER in[2] = {transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
        transition(scratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST)};
    list->ResourceBarrier(2, in);
    list->CopyResource(scratch, target);
    D3D12_RESOURCE_BARRIER toUav = transition(scratch, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->ResourceBarrier(1, &toUav);
    drawText(list, scratch, format == DXGI_FORMAT_R16G16B16A16_FLOAT);
    D3D12_RESOURCE_BARRIER back[2] = {transition(scratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        transition(target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
    list->ResourceBarrier(2, back);
    list->CopyResource(target, scratch);
    D3D12_RESOURCE_BARRIER done[2] = {transition(scratch, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        transition(target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET)};
    list->ResourceBarrier(2, done);
    restoreGameState(list, saved);
    t_inOwnWork = false;
    ReleaseSRWLockExclusive(&g_upLock);
}

// After each game draw: FSR 4 off in native mode with a notification pending -> overlay after the last stock TAA draw.
void afterGameDraw(ID3D12GraphicsCommandList* list) {
    ListRootSigs* bound = listRootSigs(list, false);
    if (!bound || !bound->overlayPending) return;
    if (bound->taaParam >= kTrackedTableParams || bound->graphicsTables[bound->taaParam] != bound->taaTable) return;
    if (++bound->overlayDraws < kTaaDrawsPerFrame) return;
    bound->overlayPending = false;
    overlayViaCopy(list, bound);
}

// After each game dispatch (when it wasn't skipped): run FSR 4 right after FSR 1's RCAS, overwriting its output.
void afterGameDispatch(ID3D12GraphicsCommandList* list, UINT x, UINT y) {
    if (!mayBeFsr1Dispatch(x, y) || g_up.broken || !g_up.color) return;
    ListRootSigs* bound = listRootSigs(list, false);
    if (!bound || !bound->compute || bound->heapCount == 0) return;
    if (!g_fsrEnabled.load(std::memory_order_relaxed)) {
        if (overlayVisible()) overlayStockRcas(list, bound);
        return;
    }
    DescEntry output{};
    // Classified even without a prepared frame, so a switch from native back to upscale mode is noticed.
    if (classifyAndNote(list, bound, &output) == Fsr1Pass::Rcas && (bound->fsrPending || g_up.lastValid)) {
        upscaleAtRcas(list, bound, output, false);
    }
}

void fillUpscaleDesc(ffxDispatchDescUpscale& d, ID3D12GraphicsCommandList* list, const PrepData& prep,
    ID3D12Resource* output, UINT outW, UINT outH) {
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = list;
    d.color = ffxApiGetResourceDX12(g_up.color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.depth = ffxApiGetResourceDX12(g_up.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors = ffxApiGetResourceDX12(g_up.mv, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    if (!prep.hdr) d.exposure = ffxApiGetResourceDX12(g_up.exposure, FFX_API_RESOURCE_STATE_COMPUTE_READ);   // HDR: auto exposure
    d.output = ffxApiGetResourceDX12(output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = {prep.jitter[0], prep.jitter[1]};
    d.motionVectorScale = {static_cast<float>(g_up.width), static_cast<float>(g_up.height)};
    d.renderSize = {g_up.width, g_up.height};
    d.upscaleSize = {outW, outH};
    d.enableSharpening = g_up.sharpness > 0.0f;
    d.sharpness = g_up.sharpness;
    d.frameTimeDelta = prep.frameTimeMs;
    d.preExposure = 1.0f;
    d.reset = prep.reset;
    d.cameraNear = prep.nearPlane;
    d.cameraFar = prep.farPlane;
    d.cameraFovAngleVertical = prep.fovY;
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = (prep.hdr ? 0u : FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB) | (g_fsrDebugView.load() ? FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW : 0u);
}

// Native mode: FSR 4 as native AA (render size = output size) at the first stock TAA draw, written to our own texture and
// copied into the TAA's render target 0 (which the rest of the frame reads). Returns false to let the stock TAA run.
bool nativeUpscale(ID3D12GraphicsCommandList* list, ListRootSigs* bound) {
    if (!g_fsrEnabled.load(std::memory_order_relaxed) || g_up.broken || !bound->fsrPending || !bound->rt0Cpu) return false;
    DescEntry rt0{};
    AcquireSRWLockShared(&g_heapLock);
    if (const DescEntry* e = findEntryByCpuLocked(bound->rt0Cpu)) rt0 = *e;
    ReleaseSRWLockShared(&g_heapLock);
    // RT0 must be the TAA's resolved output: render size, same format as the TAA colour input (RGBA8 or RGBA16F).
    if (rt0.kind != kDescRtv || !rt0.resource || rt0.width != g_up.width || rt0.height != g_up.height ||
        rt0.resFormat != static_cast<uint16_t>(g_up.colorFormat) || rt0.arraySize != 1) {
        static std::atomic<unsigned> s_logged{0};
        if (s_logged.fetch_add(1) < 4) {
            logLine("FSR_NATIVE_SKIP rt0 kind=%u %ux%u f%u (expected TAA output f%u %ux%u) -> stock TAA this frame", rt0.kind,
                rt0.width, rt0.height, rt0.resFormat, static_cast<unsigned>(g_up.colorFormat), g_up.width, g_up.height);
        }
        return false;
    }
    AcquireSRWLockExclusive(&g_upLock);
    ID3D12Device* device = nullptr;
    if (FAILED(list->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) || !device) {
        ReleaseSRWLockExclusive(&g_upLock);
        return false;
    }
    if (!g_up.nativeOut || g_up.nativeOutW != g_up.width || g_up.nativeOutH != g_up.height || g_up.nativeOutFormat != g_up.colorFormat) {
        g_up.nativeOut = createOwnTexture(device, g_up.width, g_up.height, g_up.colorFormat);  // old one leaked on purpose
        g_up.nativeOutW = g_up.width;
        g_up.nativeOutH = g_up.height;
        g_up.nativeOutFormat = g_up.colorFormat;
    }
    const bool ready = g_up.nativeOut && ensureFfxContext(device, g_up.width, g_up.height, bound->fsrPrep.hdr);
    device->Release();
    if (!ready) { ReleaseSRWLockExclusive(&g_upLock); return false; }

    const PrepData prep = bound->fsrPrep;
    bound->fsrPending = false;
    if (g_up.lastValid && g_up.last.seq == prep.seq) g_up.lastValid = false;
    ffxDispatchDescUpscale d{};
    fillUpscaleDesc(d, list, prep, g_up.nativeOut, g_up.width, g_up.height);
    auto* target = static_cast<ID3D12Resource*>(rt0.resource);

    const ListRootSigs saved = *bound;
    t_inOwnWork = true;
    const ffxReturnCode_t rc = g_up.dispatch(&g_up.context, &d.header);
    drawIndicator(list, g_up.nativeOut, true);
    drawText(list, g_up.nativeOut, prep.hdr);
    restoreGameState(list, saved);
    // nativeOut UAV -> COPY_SOURCE, TAA target RENDER_TARGET -> COPY_DEST, copy, and back.
    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Transition = {g_up.nativeOut, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COPY_SOURCE};
    barriers[1].Transition = {target, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_COPY_DEST};
    list->ResourceBarrier(2, barriers);
    list->CopyResource(target, g_up.nativeOut);
    for (D3D12_RESOURCE_BARRIER& b : barriers) std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    list->ResourceBarrier(2, barriers);
    t_inOwnWork = false;

    bound->nativeDone = true;
    ++g_up.upscaledFrames;
    ++g_up.nativeFrames;
    if (rc != FFX_API_RETURN_OK) logLine("FSR_DISPATCH rc=%u (native)", rc);
    if (g_up.nativeFrames <= kFsrMaxLoggedFrames) {
        logLine("FSR_NATIVE frame=%u list=%p target=%p %ux%u rc=%u reset=%d", g_up.nativeFrames, static_cast<void*>(list),
            rt0.resource, rt0.width, rt0.height, rc, prep.reset ? 1 : 0);
    }
    ReleaseSRWLockExclusive(&g_upLock);
    return true;
}

// Uses the frame prepared on the same list if there is one, otherwise the most recent prepared frame (FSR 1 on another list).
void upscaleAtRcas(ID3D12GraphicsCommandList* list, ListRootSigs* bound, const DescEntry& outputEntry, bool rcasSkipped) {
    const DescEntry* output = &outputEntry;
    AcquireSRWLockExclusive(&g_upLock);
    PrepData prep{};
    const char* path = nullptr;
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (bound->fsrPending) {
        prep = bound->fsrPrep;
        path = "same_list";
        ++g_up.sameList;
    } else if (g_up.lastValid && 1000.0 * (now.QuadPart - g_up.last.qpc) / freq.QuadPart <= kCrossListPrepMaxAgeMs) {
        prep = g_up.last;
        path = "other_list";
        ++g_up.otherList;
    } else {
        const unsigned noPrep = ++g_up.noPrep;
        if (noPrep <= 4 || noPrep % 600 == 0) {
            logLine("FSR_RCAS_NO_PREP #%u list=%p last_valid=%d", noPrep, static_cast<void*>(list), g_up.lastValid ? 1 : 0);
        }
        ReleaseSRWLockExclusive(&g_upLock);
        return;
    }
    bound->fsrPending = false;
    if (g_up.lastValid && g_up.last.seq == prep.seq) g_up.lastValid = false;
    // Clean same-list pairing frames enable stock skipping; anything else resets it.
    if (path[0] == 's') {
        if (++g_up.stableFrames == kStableFramesForSkip && g_up.skipStock) logLine("FSR_SKIP_ACTIVE after %u clean frames", kStableFramesForSkip);
    } else {
        if (g_up.stableFrames >= kStableFramesForSkip) logLine("FSR_SKIP_OFF reason=other_list_pairing");
        g_up.stableFrames = 0;
    }
    const unsigned accepted = g_up.sameList + g_up.otherList;
    if (accepted <= 4 || accepted % 600 == 0) {
        logLine("FSR_RCAS path=%s list=%p prep_list=%p prep_age_ms=%.2f seq=%u", path, static_cast<void*>(list), g_up.lastList,
            1000.0 * (now.QuadPart - prep.qpc) / freq.QuadPart, prep.seq);
    }

    ID3D12Device* device = nullptr;
    if (FAILED(list->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) || !device) {
        ReleaseSRWLockExclusive(&g_upLock);
        return;
    }
    const bool ready = ensureFfxContext(device, output->width, output->height, prep.hdr);
    device->Release();
    if (!ready) { ReleaseSRWLockExclusive(&g_upLock); return; }

    auto* outputResource = static_cast<ID3D12Resource*>(output->resource);
    ffxDispatchDescUpscale d{};
    fillUpscaleDesc(d, list, prep, outputResource, output->width, output->height);

    // F6: same-frame A/B capture (FSR 1 result before we overwrite it, FSR 4 result after, plus our inputs).
    AbJob* ab = nullptr;
    if (g_abArmed.exchange(false)) {
        ab = new (std::nothrow) AbJob;
        if (ab) { ab->index = g_abIndex.fetch_add(1) + 1; ab->list = list; }
        else g_abBusy.store(false);
    }

    const ListRootSigs saved = *bound;
    t_inOwnWork = true;
    D3D12_RESOURCE_BARRIER uavBarrier{};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = outputResource;   // RCAS just wrote it (unless skipped); FFX writes it next.
    if (!rcasSkipped) list->ResourceBarrier(1, &uavBarrier);
    if (ab) recordReadback(list, ab, outputResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "fsr1");
    const ffxReturnCode_t rc = g_up.dispatch(&g_up.context, &d.header);
    if (!ab) {   // Not into A/B captures (would skew the comparison).
        drawIndicator(list, outputResource, false);
        drawText(list, outputResource, prep.hdr);
    }
    restoreGameState(list, saved);
    if (ab) {
        list->ResourceBarrier(1, &uavBarrier);
        recordReadback(list, ab, outputResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "fsr4");
        recordReadback(list, ab, g_up.mv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "mv");
        recordReadback(list, ab, g_up.color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "input");
        char meta[256];
        snprintf(meta, sizeof(meta), "jitter=%.4f,%.4f near=%.4f far=%.1f fov=%.4f dt=%.2f reset=%d rc=%u debug_view=%d upscaled_frames=%u path=%s\r\n",
            prep.jitter[0], prep.jitter[1], prep.nearPlane, prep.farPlane, prep.fovY, prep.frameTimeMs, d.reset ? 1 : 0, rc,
            g_fsrDebugView.load() ? 1 : 0, g_up.upscaledFrames, path);
        ab->meta = meta;
        ab->recordedTick = GetTickCount64();
        g_abRecorded.store(ab);
        logLine("FSR_AB index=%u recorded items=%u", ab->index, ab->count);
    }
    t_inOwnWork = false;
    if (rc != FFX_API_RETURN_OK) logLine("FSR_DISPATCH rc=%u", rc);
    ++g_up.upscaledFrames;
    if (g_up.upscaledFrames <= kFsrMaxLoggedFrames) {
        logLine("FSR_UPSCALE frame=%u list=%p output=%p %ux%u rc=%u reset=%d", g_up.upscaledFrames, static_cast<void*>(list),
            output->resource, output->width, output->height, rc, d.reset ? 1 : 0);
    } else if (g_up.upscaledFrames % 1800 == 0) {
        logLine("FSR_STATS upscaled=%u prepared=%u", g_up.upscaledFrames, g_up.preparedFrames);
    }
    ReleaseSRWLockExclusive(&g_upLock);
}

DWORD WINAPI abWriterThread(LPVOID param) {
    AbJob* job = static_cast<AbJob*>(param);
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool ready = false;
    if (event && SUCCEEDED(g_abFence->SetEventOnCompletion(job->fenceValue, event))) {
        ready = WaitForSingleObject(event, 10000) == WAIT_OBJECT_0;
    }
    if (event) CloseHandle(event);
    wchar_t dir[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, ARRAYSIZE(dir));
    wchar_t sub[32]{};
    StringCchPrintfW(sub, ARRAYSIZE(sub), L"\\ACVFSR31\\ab_%u", job->index);
    if (ready && length != 0 && length < ARRAYSIZE(dir) - 40 && SUCCEEDED(StringCchCatW(dir, ARRAYSIZE(dir), sub))) {
        CreateDirectoryW(dir, nullptr);
        std::string meta = job->meta;
        for (UINT i = 0; i < job->count; ++i) {
            const AbItem& item = job->items[i];
            wchar_t path[MAX_PATH]{};
            StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%S_f%u_%ux%u.raw", dir, item.name, item.format,
                item.footprint.Footprint.Width, item.footprint.Footprint.Height);
            void* mapped = nullptr;
            const D3D12_RANGE readRange{0, static_cast<SIZE_T>(item.footprint.Footprint.RowPitch) * item.rows};
            if (FAILED(item.readback->Map(0, &readRange, &mapped)) || !mapped) continue;
            const HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                for (UINT row = 0; row < item.rows; ++row) {
                    DWORD written = 0;
                    WriteFile(file, static_cast<const uint8_t*>(mapped) + static_cast<SIZE_T>(row) * item.footprint.Footprint.RowPitch,
                        static_cast<DWORD>(item.rowBytes), &written, nullptr);
                }
                CloseHandle(file);
            }
            const D3D12_RANGE noWrite{0, 0};
            item.readback->Unmap(0, &noWrite);
        }
        wchar_t metaPath[MAX_PATH]{};
        StringCchPrintfW(metaPath, ARRAYSIZE(metaPath), L"%s\\ab.txt", dir);
        const HANDLE file = CreateFileW(metaPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, meta.data(), static_cast<DWORD>(meta.size()), &written, nullptr);
            CloseHandle(file);
        }
        logLine("FSR_AB index=%u written", job->index);
    } else {
        logLine("FSR_AB index=%u error=%s", job->index, ready ? "path" : "gpu_timeout");
    }
    MessageBeep(MB_OK);
    for (UINT i = 0; i < job->count; ++i) job->items[i].readback->Release();
    delete job;
    g_abBusy.store(false);
    return 0;
}

// Called from ExecuteCommandLists after the original submit (like onSubmitForDump).
void freeAbJob(AbJob* job, const char* reason) {
    logLine("FSR_AB index=%u error=%s", job->index, reason);
    for (UINT k = 0; k < job->count; ++k) job->items[k].readback->Release();
    delete job;
    g_abBusy.store(false);
}

void onSubmitForAb(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    AbJob* job = g_abRecorded.load();
    if (!job) return;
    for (UINT i = 0; i < count; ++i) {
        if (static_cast<void*>(lists[i]) != job->list) continue;
        if (g_abRecorded.exchange(nullptr) != job) return;
        job->fenceValue = ++g_abFenceValue;
        if (FAILED(queue->Signal(g_abFence, job->fenceValue))) { freeAbJob(job, "signal"); return; }
        const HANDLE thread = CreateThread(nullptr, 0, &abWriterThread, job, 0, nullptr);
        if (thread) CloseHandle(thread);
        else freeAbJob(job, "thread");
        return;
    }
    // The recorded list was never submitted (reset and re-recorded): give up after a while.
    if (GetTickCount64() - job->recordedTick > kDumpSubmitTimeoutMs && g_abRecorded.exchange(nullptr) == job) {
        freeAbJob(job, "not_submitted");
    }
}

bool pollKeyEdge(int key, std::atomic<bool>& wasDown) {
    const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
    if (!down) { wasDown.store(false); return false; }
    return !wasDown.exchange(true);
}

// Logs when FSR 4 is on but has stopped upscaling for a while (and when it resumes), so a silent stop shows in the log.
void watchFsrActivity() {
    static unsigned s_lastCount = 0;
    static ULONGLONG s_lastChange = 0;
    static bool s_idle = false;
    static ULONGLONG s_lastRate = 0;
    static unsigned s_ratePrepared = 0, s_rateUpscaled = 0, s_rateFrames = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - s_lastRate >= 5000) {
        // Frame rate per mode (one TAA match per frame in the world): compare stock (F7 off), fsr4 (F4 off) and fsr4+skip.
        if (s_lastRate && g_up.taaFrames != s_rateFrames) {
            const double seconds = (now - s_lastRate) / 1000.0;
            const double fps = (g_up.taaFrames - s_rateFrames) / seconds;
            const char* mode = !g_fsrEnabled.load() ? "stock"
                : g_up.nativeMode ? "fsr4-native"
                : (g_up.skipStock && g_up.stableFrames >= kStableFramesForSkip) ? "fsr4+skip" : "fsr4";
            logLine("FSR_PERF mode=%s fps=%.1f frame_ms=%.2f skipped_taa_draws=%u skipped_fsr1=%u", mode, fps,
                fps > 0 ? 1000.0 / fps : 0.0, g_up.skippedTaa, g_up.skippedFsr1);
        }
        s_rateFrames = g_up.taaFrames;
        if (s_lastRate && g_up.preparedFrames != s_ratePrepared) {
            const double seconds = (now - s_lastRate) / 1000.0;
            logLine("FSR_RATE prepared/s=%.1f upscaled/s=%.1f fsr1_candidates=%u rejected(easu)=%u same_list=%u other_list=%u "
                "no_prep=%u orphans=%u enabled=%d",
                (g_up.preparedFrames - s_ratePrepared) / seconds, (g_up.upscaledFrames - s_rateUpscaled) / seconds,
                g_up.candidates, g_up.rejected, g_up.sameList, g_up.otherList, g_up.noPrep, g_up.orphanPreps,
                g_fsrEnabled.load() ? 1 : 0);
        }
        s_lastRate = now;
        s_ratePrepared = g_up.preparedFrames;
        s_rateUpscaled = g_up.upscaledFrames;
    }
    const unsigned count = g_up.upscaledFrames;
    if (count != s_lastCount) {
        if (s_idle) logLine("FSR_RESUMED upscaled=%u prepared=%u", count, g_up.preparedFrames);
        s_idle = false;
        s_lastCount = count;
        s_lastChange = now;
        return;
    }
    if (!s_idle && count > 0 && g_fsrEnabled.load() && !g_up.broken && now - s_lastChange > 2000) {
        s_idle = true;
        logLine("FSR_IDLE no upscaled frame for 2 s (upscaled=%u prepared=%u orphan_preps=%u)", count, g_up.preparedFrames,
            g_up.orphanPreps);
    }
}

// F7 toggles FSR 4, F6 arms a same-frame A/B capture, F5 toggles FSR's on-screen debug view.
// Release build: sharpness, stock-skip and indicator settings persist in %LOCALAPPDATA%\ACVFSR4\settings.ini.
bool settingsPath(wchar_t* path, size_t size) {
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", path, static_cast<DWORD>(size));
    if (length == 0 || length >= size - 40) return false;
    if (FAILED(StringCchCatW(path, size, L"\\ACVFSR4"))) return false;
    CreateDirectoryW(path, nullptr);
    return SUCCEEDED(StringCchCatW(path, size, L"\\settings.ini"));
}

void loadSettings() {
    static bool s_loaded = false;
    if (s_loaded || !kRelease) return;
    s_loaded = true;
    wchar_t path[MAX_PATH]{};
    if (!settingsPath(path, ARRAYSIZE(path))) return;
    wchar_t value[32]{};
    if (GetPrivateProfileStringW(L"fsr4", L"sharpness", L"", value, ARRAYSIZE(value), path) > 0) {
        const float s = static_cast<float>(_wtof(value));
        if (s >= 0.0f && s <= 1.0f) g_up.sharpness = s;
    }
    g_up.skipStock = GetPrivateProfileIntW(L"fsr4", L"skip_stock", 1, path) != 0;
    g_up.indicatorAlways = GetPrivateProfileIntW(L"fsr4", L"indicator_always", 0, path) != 0;
    const UINT phases = GetPrivateProfileIntW(L"fsr4", L"jitter_phases", 8, path);
    g_up.jitterPhases = phases == 16 || phases == 32 ? phases : 8;
    logLine("FSR_SETTINGS sharpness=%.1f skip_stock=%d indicator_always=%d jitter_phases=%u", g_up.sharpness, g_up.skipStock ? 1 : 0,
        g_up.indicatorAlways ? 1 : 0, g_up.jitterPhases);
}

void saveSettings() {
    if (!kRelease) return;
    wchar_t path[MAX_PATH]{};
    if (!settingsPath(path, ARRAYSIZE(path))) return;
    wchar_t value[32]{};
    StringCchPrintfW(value, ARRAYSIZE(value), L"%.1f", g_up.sharpness);
    WritePrivateProfileStringW(L"fsr4", L"sharpness", value, path);
    WritePrivateProfileStringW(L"fsr4", L"skip_stock", g_up.skipStock ? L"1" : L"0", path);
    WritePrivateProfileStringW(L"fsr4", L"indicator_always", g_up.indicatorAlways ? L"1" : L"0", path);
    StringCchPrintfW(value, ARRAYSIZE(value), L"%u", g_up.jitterPhases);
    WritePrivateProfileStringW(L"fsr4", L"jitter_phases", value, path);
}

void notifySharpness() {
    char text[kTextMaxChars + 1];
    snprintf(text, sizeof(text), "FSR 4 SHARPNESS %.1f", g_up.sharpness);
    notify(text, 1.0f, 0.85f, 0.2f);
}

// ---------- jitter table scan (probe build, opt-in via acvfsr_jitter_scan.txt next to the game exe) ----------
// The game cycles 8 jitter phases picked on the CPU. If they come from a table in memory, rewriting that table could give
// FSR 4 more phases without patching game code. This only looks: after enough prepared frames a low-priority thread
// searches the process memory for runs of the observed jitter values (and common transforms of them) and logs them.
constexpr UINT kJitterHistory = 64;
constexpr UINT kJitterScanDefaultFrames = 900;
constexpr UINT kJitterMaxValues = 16;
constexpr UINT kJitterMaxPatterns = 16;
constexpr UINT kJitterMaxMatches = 64;
constexpr UINT kJitterMatchFloats = 16;
constexpr SIZE_T kJitterMaxRegion = 512ull << 20;
constexpr UINT kJitterStrides[] = {1, 2, 3, 4, 6, 8};

struct JitterHistory {
    float x[kJitterHistory];
    float y[kJitterHistory];
    UINT count;
    UINT width, height;
};
JitterHistory g_jitter{};   // Render thread only; the scan thread gets a copy.
std::atomic<bool> g_jitterScanStarted{false};

struct JitterPattern {
    char name[40];
    float values[kJitterMaxValues];
    UINT count;
    float lo, hi;
};

struct JitterMatch {
    const float* at;
    UINT pattern;
    UINT stride;
    float values[kJitterMatchFloats];
};

// Everything the scan thread uses lives in its own VirtualAlloc region, which the scan skips (it holds the values too).
struct JitterScan {
    JitterHistory history;
    float xs[kJitterMaxValues], ys[kJitterMaxValues];
    JitterPattern patterns[kJitterMaxPatterns];
    UINT patternCount;
    float lo, hi;   // Over all patterns.
    JitterMatch matches[kJitterMaxMatches];
    UINT matchCount;
    UINT totalMatches;
};

void jitterMapFrame(float x, float y);

// Both builds (the release build uses the history to log how many jitter phases the game really cycles).
void recordJitter(float x, float y, UINT width, UINT height) {
    const UINT i = g_jitter.count++ % kJitterHistory;
    g_jitter.x[i] = x;
    g_jitter.y[i] = y;
    g_jitter.width = width;
    g_jitter.height = height;
    if (!kRelease) jitterMapFrame(x, y);
}

int jitterIndex(const JitterPattern& pattern, float v) {
    if (!(v >= pattern.lo && v <= pattern.hi)) return -1;   // Also rejects NaN.
    for (UINT k = 0; k < pattern.count; ++k) {
        const float want = pattern.values[k];
        if (std::fabs(v - want) <= 1e-4f * std::fabs(want) + 1e-7f) return static_cast<int>(k);
    }
    return -1;
}

// SEH only (no C++ objects): false if the region faulted while being read (freed or re-protected by the game meanwhile).
bool scanJitterRegion(const float* begin, SIZE_T count, JitterScan* scan) {
    __try {
        for (SIZE_T i = 0; i < count; ++i) {
            const float v = begin[i];
            if (!(v >= scan->lo && v <= scan->hi)) continue;
            if (std::fabs(v) < 1e-6f) {
                // Zero (most of memory): only worth a look if a later element at some stride is a non-zero candidate.
                bool next = false;
                for (UINT stride : kJitterStrides) {
                    if (i + stride >= count) break;
                    const float w = begin[i + stride];
                    next = next || (w >= scan->lo && w <= scan->hi && std::fabs(w) >= 1e-6f);
                }
                if (!next) continue;
            }
            for (UINT p = 0; p < scan->patternCount; ++p) {
                const JitterPattern& pattern = scan->patterns[p];
                if (jitterIndex(pattern, v) < 0) continue;
                for (UINT stride : kJitterStrides) {
                    if (i + static_cast<SIZE_T>(pattern.count - 1) * stride >= count) continue;
                    if (i >= stride && jitterIndex(pattern, begin[i - stride]) >= 0) continue;   // Not the start of the run.
                    UINT seen = 0;
                    for (SIZE_T j = i; j < count; j += stride) {
                        const int k = jitterIndex(pattern, begin[j]);
                        if (k < 0) break;
                        seen |= 1u << k;
                    }
                    if (seen != (1u << pattern.count) - 1u) continue;
                    if (scan->matchCount < kJitterMaxMatches) {
                        JitterMatch& m = scan->matches[scan->matchCount++];
                        m.at = begin + i;
                        m.pattern = p;
                        m.stride = stride;
                        for (UINT k = 0; k < kJitterMatchFloats; ++k) m.values[k] = i + k < count ? begin[i + k] : 0.0f;
                    }
                    ++scan->totalMatches;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

void addJitterPattern(JitterScan* scan, const char* name, const float* values, UINT count, float scale, float offset) {
    if (scan->patternCount >= kJitterMaxPatterns || count < 3) return;
    JitterPattern& pattern = scan->patterns[scan->patternCount];
    StringCchCopyA(pattern.name, ARRAYSIZE(pattern.name), name);
    pattern.count = count;
    pattern.lo = 1e30f;
    pattern.hi = -1e30f;
    for (UINT k = 0; k < count; ++k) {
        const float v = values[k] * scale + offset;
        pattern.values[k] = v;
        const float tol = 1e-4f * std::fabs(v) + 1e-7f;
        pattern.lo = (std::min)(pattern.lo, v - tol);
        pattern.hi = (std::max)(pattern.hi, v + tol);
    }
    // A symmetric set (e.g. y = +-1/18 ...) equals its negation: keep only the first of identical sets.
    for (UINT p = 0; p < scan->patternCount; ++p) {
        const JitterPattern& other = scan->patterns[p];
        bool same = other.count == count;
        for (UINT k = 0; same && k < count; ++k) same = jitterIndex(other, pattern.values[k]) >= 0;
        if (same) return;
    }
    scan->lo = scan->patternCount ? (std::min)(scan->lo, pattern.lo) : pattern.lo;
    scan->hi = scan->patternCount ? (std::max)(scan->hi, pattern.hi) : pattern.hi;
    ++scan->patternCount;
}

UINT distinctJitter(const float* history, UINT available, float* out) {
    UINT count = 0;
    for (UINT i = 0; i < available && count < kJitterMaxValues; ++i) {
        bool known = false;
        for (UINT k = 0; k < count && !known; ++k) known = std::fabs(out[k] - history[i]) < 1e-5f;
        if (!known) out[count++] = history[i];
    }
    return count;
}

DWORD WINAPI jitterScanThread(void* param) {
    auto* scan = static_cast<JitterScan*>(param);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    const JitterHistory& h = scan->history;
    const UINT available = (std::min)(h.count, kJitterHistory);
    const UINT newest = h.count - 1;

    // The observed sequence (oldest first, last 24) and its period.
    char line[512];
    int used = snprintf(line, sizeof(line), "JITTER_SEQ frames=%u size=%ux%u", h.count, h.width, h.height);
    for (UINT n = (std::min)(available, 24u); n > 0 && used < static_cast<int>(sizeof(line)) - 24; --n) {
        const UINT i = (newest - (n - 1)) % kJitterHistory;
        used += snprintf(line + used, sizeof(line) - used, " (%.4f,%.4f)", h.x[i], h.y[i]);
    }
    logLine("%s", line);
    UINT period = 0;
    for (UINT p = 1; p <= 32 && !period; ++p) {
        bool repeats = available > p + 8;
        for (UINT n = 0; repeats && n + p < available; ++n) {
            const UINT a = (newest - n) % kJitterHistory, b = (newest - n - p) % kJitterHistory;
            repeats = std::fabs(h.x[a] - h.x[b]) < 1e-5f && std::fabs(h.y[a] - h.y[b]) < 1e-5f;
        }
        if (repeats) period = p;
    }

    float* xs = scan->xs;   // In the scan block (skipped), not on this thread's stack (which would match).
    float* ys = scan->ys;
    const UINT nx = distinctJitter(h.x, available, xs), ny = distinctJitter(h.y, available, ys);
    logLine("JITTER_SCAN start period=%u distinct_x=%u distinct_y=%u", period, nx, ny);
    const float w = static_cast<float>(h.width ? h.width : 1), hh = static_cast<float>(h.height ? h.height : 1);
    addJitterPattern(scan, "x_pixels", xs, nx, 1.0f, 0.0f);
    addJitterPattern(scan, "y_pixels", ys, ny, 1.0f, 0.0f);
    addJitterPattern(scan, "x_neg", xs, nx, -1.0f, 0.0f);
    addJitterPattern(scan, "y_neg", ys, ny, -1.0f, 0.0f);
    addJitterPattern(scan, "x_plus_half", xs, nx, 1.0f, 0.5f);
    addJitterPattern(scan, "y_plus_half", ys, ny, 1.0f, 0.5f);
    addJitterPattern(scan, "x_half_minus", xs, nx, -1.0f, 0.5f);
    addJitterPattern(scan, "y_half_minus", ys, ny, -1.0f, 0.5f);
    addJitterPattern(scan, "x_times2", xs, nx, 2.0f, 0.0f);
    addJitterPattern(scan, "y_times2", ys, ny, 2.0f, 0.0f);
    addJitterPattern(scan, "x_ndc", xs, nx, 2.0f / w, 0.0f);
    addJitterPattern(scan, "y_ndc", ys, ny, 2.0f / hh, 0.0f);
    addJitterPattern(scan, "y_ndc_neg", ys, ny, -2.0f / hh, 0.0f);
    addJitterPattern(scan, "x_uv", xs, nx, 1.0f / w, 0.0f);
    addJitterPattern(scan, "y_uv", ys, ny, 1.0f / hh, 0.0f);

    // Our own DLL image (static history) is skipped as well as the scan block itself.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&g_jitter), &self);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(self) + dos->e_lfanew);
    const ULONG_PTR selfBegin = reinterpret_cast<ULONG_PTR>(self), selfEnd = selfBegin + nt->OptionalHeader.SizeOfImage;

    LARGE_INTEGER t0{}, t1{}, freq{};
    QueryPerformanceCounter(&t0);
    QueryPerformanceFrequency(&freq);
    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
        PAGE_EXECUTE_WRITECOPY;
    unsigned long long scannedBytes = 0;
    UINT regions = 0, skippedLarge = 0, faulted = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    for (const BYTE* at = nullptr; VirtualQuery(at, &mbi, sizeof(mbi)) == sizeof(mbi);
         at = static_cast<const BYTE*>(mbi.BaseAddress) + mbi.RegionSize) {
        const ULONG_PTR base = reinterpret_cast<ULONG_PTR>(mbi.BaseAddress);
        if (mbi.State != MEM_COMMIT || !(mbi.Protect & kReadable) || (mbi.Protect & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE))) continue;
        if (base >= selfBegin && base < selfEnd) continue;
        if (mbi.AllocationBase == static_cast<void*>(scan)) continue;
        if (mbi.RegionSize > kJitterMaxRegion) { ++skippedLarge; continue; }
        ++regions;
        scannedBytes += mbi.RegionSize;
        if (!scanJitterRegion(static_cast<const float*>(mbi.BaseAddress), mbi.RegionSize / sizeof(float), scan)) ++faulted;
    }
    QueryPerformanceCounter(&t1);

    for (UINT i = 0; i < scan->matchCount; ++i) {
        const JitterMatch& m = scan->matches[i];
        wchar_t moduleName[MAX_PATH] = L"-";
        HMODULE module = nullptr;
        unsigned long long offset = 0;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(m.at), &module) && module) {
            wchar_t full[MAX_PATH]{};
            GetModuleFileNameW(module, full, ARRAYSIZE(full));
            const wchar_t* slash = wcsrchr(full, L'\\');
            StringCchCopyW(moduleName, ARRAYSIZE(moduleName), slash ? slash + 1 : full);
            offset = reinterpret_cast<ULONG_PTR>(m.at) - reinterpret_cast<ULONG_PTR>(module);
        }
        MEMORY_BASIC_INFORMATION where{};
        VirtualQuery(m.at, &where, sizeof(where));
        const char* type = where.Type == MEM_IMAGE ? "image" : (where.Type == MEM_MAPPED ? "mapped" : "private");
        used = snprintf(line, sizeof(line), "JITTER_SCAN match pattern=%s stride=%u addr=%p module=%ls+0x%llx type=%s protect=0x%lx values=",
            scan->patterns[m.pattern].name, m.stride, static_cast<const void*>(m.at), moduleName, offset, type, where.Protect);
        for (UINT k = 0; k < kJitterMatchFloats && used < static_cast<int>(sizeof(line)) - 16; ++k) {
            used += snprintf(line + used, sizeof(line) - used, "%s%.6g", k ? "," : "", m.values[k]);
        }
        logLine("%s", line);
    }
    logLine("JITTER_SCAN done matches=%u logged=%u regions=%u scanned_mb=%llu skipped_large=%u faulted=%u seconds=%.1f",
        scan->totalMatches, scan->matchCount, regions, scannedBytes >> 20, skippedLarge, faulted,
        static_cast<double>(t1.QuadPart - t0.QuadPart) / freq.QuadPart);
    MessageBeep(MB_ICONEXCLAMATION);
    VirtualFree(scan, 0, MEM_RELEASE);
    return 0;
}

// Probe opt-in marker next to the game exe: false when missing, else its first line (may be empty) in text.
bool readMarker(const wchar_t* name, char* text, DWORD size) {
    if (kRelease) return false;
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    wchar_t* slash = length ? wcsrchr(path, L'\\') : nullptr;
    if (!slash) return false;
    slash[1] = L'\0';
    if (FAILED(StringCchCatW(path, ARRAYSIZE(path), name))) return false;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::memset(text, 0, size);
    DWORD read = 0;
    ReadFile(file, text, size - 1, &read, nullptr);
    CloseHandle(file);
    return true;
}

// Frames before the scan: the number in acvfsr_jitter_scan.txt (default 900), 0 = no marker file = off.
UINT jitterScanFrames() {
    char text[16];
    if (!readMarker(L"acvfsr_jitter_scan.txt", text, sizeof(text))) return 0;
    const int frames = atoi(text);
    return frames > 0 ? static_cast<UINT>(frames) : kJitterScanDefaultFrames;
}

void pollJitterScan() {
    static const UINT s_frames = jitterScanFrames();
    if (!s_frames || g_jitter.count < s_frames || g_jitterScanStarted.exchange(true)) return;
    auto* scan = static_cast<JitterScan*>(VirtualAlloc(nullptr, sizeof(JitterScan), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!scan) { logLine("JITTER_SCAN error=alloc"); return; }
    scan->history = g_jitter;
    logLine("JITTER_SCAN queued after %u prepared frames", g_jitter.count);
    MessageBeep(MB_OK);
    HANDLE thread = CreateThread(nullptr, 0, jitterScanThread, scan, 0, nullptr);
    if (thread) CloseHandle(thread);
    else { logLine("JITTER_SCAN error=thread"); VirtualFree(scan, 0, MEM_RELEASE); }
}

// ---------- jitter table map (probe build, opt-in via acvfsr_jitter_map.txt = table RVA in hex) ----------
// The scan found the 8 jitter pairs in ACValhalla.exe's writable data. This maps the neighbourhood: one dump of +-4 KB around
// the table, which dwords in it change frame to frame (phase index / counters), and the game code whose RIP-relative operands
// point near the table (for offline disassembly). Read-only; written to %LOCALAPPDATA%\ACVFSR31\jitter_map.txt.
constexpr ULONG_PTR kJitterMapDefaultRva = 0x61a2950;
constexpr UINT kJitterMapWindow = 4096;              // Bytes on each side of the table.
constexpr UINT kJitterMapStartFrames = 300;          // Prepared frames before the watch starts.
constexpr UINT kJitterMapFrames = 96;                // Snapshots, one per prepared frame.
constexpr UINT kJitterMapMaxRefs = 512;
constexpr UINT kJitterMapCodeBytes = 192;            // Code bytes kept around each reference (half before).
constexpr int kJitterMapRefLo = -256, kJitterMapRefHi = 1024;   // Reference targets kept, relative to the table.

struct JitterCodeRef {
    ULONG_PTR at;        // Address of the disp32.
    ULONG_PTR target;
    UINT tail;           // Immediate bytes assumed after the disp32.
    ULONG_PTR start;     // Address of code[0] (rva - half, clamped to the section).
    BYTE code[kJitterMapCodeBytes];
};

struct JitterMap {
    ULONG_PTR table = 0;
    UINT frames = 0;                                  // Snapshots taken.
    UINT faulted = 0;
    std::vector<BYTE> snapshots;                      // kJitterMapFrames x 2 * kJitterMapWindow.
    float jitter[kJitterMapFrames][2]{};
    bool started = false;
};
JitterMap g_jitterMap;   // Render thread until the snapshots are full, then the writer thread.

// SEH only: copy readable memory, false on a fault.
bool copyGuarded(void* dst, const void* src, SIZE_T size) {
    __try {
        std::memcpy(dst, src, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// SEH only: RIP-relative operands ([rip + disp32], ModRM mod=00 rm=101) in [begin, begin+size) whose target lies in [lo, hi).
UINT findCodeRefs(const BYTE* begin, SIZE_T size, ULONG_PTR lo, ULONG_PTR hi, JitterCodeRef* refs, UINT maxRefs, UINT* total) {
    UINT count = 0;
    __try {
        for (SIZE_T p = 1; p + 4 <= size; ++p) {
            if ((begin[p - 1] & 0xC7) != 0x05) continue;
            const INT32 disp = *reinterpret_cast<const INT32*>(begin + p);
            for (UINT tail : {0u, 1u, 4u}) {
                const ULONG_PTR target = reinterpret_cast<ULONG_PTR>(begin + p + 4 + tail) + static_cast<LONG_PTR>(disp);
                if (target < lo || target >= hi) continue;
                ++*total;
                if (count < maxRefs) {
                    JitterCodeRef& r = refs[count++];
                    r.at = reinterpret_cast<ULONG_PTR>(begin + p);
                    r.target = target;
                    r.tail = tail;
                    const SIZE_T from = p >= kJitterMapCodeBytes / 2 ? p - kJitterMapCodeBytes / 2 : 0;
                    r.start = reinterpret_cast<ULONG_PTR>(begin + from);
                    for (UINT k = 0; k < kJitterMapCodeBytes; ++k) r.code[k] = from + k < size ? begin[from + k] : 0;
                }
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return count;
}

void appendf(std::string& out, const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (n > 0) out.append(buffer, static_cast<size_t>((std::min)(n, static_cast<int>(sizeof(buffer)) - 1)));
}

DWORD WINAPI jitterMapThread(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    JitterMap& m = g_jitterMap;
    const BYTE* exe = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(exe + reinterpret_cast<const IMAGE_DOS_HEADER*>(exe)->e_lfanew);
    const UINT span = 2 * kJitterMapWindow;
    const BYTE* first = m.snapshots.data();
    auto dwordAt = [&](UINT frame, UINT offset) {
        UINT32 v;
        std::memcpy(&v, m.snapshots.data() + static_cast<size_t>(frame) * span + offset, 4);
        return v;
    };
    auto floatOf = [](UINT32 v) { float f; std::memcpy(&f, &v, 4); return f; };

    std::string out;
    appendf(out, "exe=%p size=0x%lx table=%p rva=0x%llx window=+-%u frames=%u faulted=%u\r\n", static_cast<const void*>(exe),
        nt->OptionalHeader.SizeOfImage, reinterpret_cast<void*>(m.table), static_cast<unsigned long long>(m.table - reinterpret_cast<ULONG_PTR>(exe)),
        kJitterMapWindow, m.frames, m.faulted);

    // Phase per frame: index of the frame's jitter among the table's first 16 pairs.
    out += "PHASES (frame: jitter -> pair index in the table, -1 = not found)\r\n";
    for (UINT f = 0; f < m.frames; ++f) {
        int phase = -1;
        for (UINT k = 0; k < 16 && phase < 0; ++k) {
            const float tx = floatOf(dwordAt(f, kJitterMapWindow + k * 8)), ty = floatOf(dwordAt(f, kJitterMapWindow + k * 8 + 4));
            if (std::fabs(tx - m.jitter[f][0]) < 1e-4f && std::fabs(ty - m.jitter[f][1]) < 1e-4f) phase = static_cast<int>(k);
        }
        appendf(out, "%u: (%.4f,%.4f) -> %d\r\n", f, m.jitter[f][0], m.jitter[f][1], phase);
    }

    // Dwords that change during the watch, with their values per frame.
    out += "CHANGES (offset from table: distinct values, then value per frame as hex)\r\n";
    UINT changed = 0;
    for (UINT offset = 0; offset + 4 <= span; offset += 4) {
        UINT distinct = 1;
        for (UINT f = 1; f < m.frames; ++f) {
            bool seen = false;
            for (UINT g = 0; g < f && !seen; ++g) seen = dwordAt(f, offset) == dwordAt(g, offset);
            if (!seen) ++distinct;
        }
        if (distinct == 1) continue;
        ++changed;
        appendf(out, "%+d: distinct=%u", static_cast<int>(offset) - static_cast<int>(kJitterMapWindow), distinct);
        for (UINT f = 0; f < m.frames; ++f) appendf(out, " %x", dwordAt(f, offset));
        out += "\r\n";
    }
    appendf(out, "changed_dwords=%u\r\n", changed);

    // Full first snapshot: offset, 4 dwords hex, same as floats.
    out += "DATA (offset from table: 4 dwords hex | floats)\r\n";
    for (UINT offset = 0; offset < span; offset += 16) {
        UINT32 d[4];
        std::memcpy(d, first + offset, 16);
        appendf(out, "%+6d: %08x %08x %08x %08x | %g %g %g %g\r\n", static_cast<int>(offset) - static_cast<int>(kJitterMapWindow),
            d[0], d[1], d[2], d[3], floatOf(d[0]), floatOf(d[1]), floatOf(d[2]), floatOf(d[3]));
    }

    // Code with RIP-relative operands pointing near the table, in every executable section of the exe.
    std::vector<JitterCodeRef> refs(kJitterMapMaxRefs);
    UINT refCount = 0, refTotal = 0;
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (UINT s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        char name[9]{};
        std::memcpy(name, section->Name, 8);
        const UINT before = refTotal;
        refCount += findCodeRefs(exe + section->VirtualAddress, section->Misc.VirtualSize, m.table + kJitterMapRefLo,
            m.table + kJitterMapRefHi, refs.data() + refCount, kJitterMapMaxRefs - refCount, &refTotal);
        appendf(out, "SECTION %s rva=0x%lx size=0x%lx refs=%u\r\n", name, section->VirtualAddress, section->Misc.VirtualSize, refTotal - before);
    }
    // Closest to the table first (the table itself, then its neighbours); the file keeps them all up to the cap.
    std::sort(refs.begin(), refs.begin() + refCount, [&](const JitterCodeRef& a, const JitterCodeRef& b) {
        const LONG_PTR da = static_cast<LONG_PTR>(a.target - m.table), db = static_cast<LONG_PTR>(b.target - m.table);
        const LONG_PTR ka = da >= 0 ? da : 4 * -da, kb = db >= 0 ? db : 4 * -db;   // Before the table counts less.
        return ka != kb ? ka < kb : a.at < b.at;
    });
    appendf(out, "CODEREFS total=%u kept=%u (rva of disp32, target offset from table, imm tail, rva of the first of %u bytes)\r\n",
        refTotal, refCount, kJitterMapCodeBytes);
    for (UINT i = 0; i < refCount; ++i) {
        const JitterCodeRef& r = refs[i];
        appendf(out, "REF rva=0x%llx target=%+lld tail=%u from=0x%llx bytes=", static_cast<unsigned long long>(r.at - reinterpret_cast<ULONG_PTR>(exe)),
            static_cast<long long>(r.target - m.table), r.tail, static_cast<unsigned long long>(r.start - reinterpret_cast<ULONG_PTR>(exe)));
        for (UINT k = 0; k < kJitterMapCodeBytes; ++k) appendf(out, "%02x", r.code[k]);
        out += "\r\n";
    }

    wchar_t path[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", path, ARRAYSIZE(path));
    if (length && length < ARRAYSIZE(path) - 40 && SUCCEEDED(StringCchCatW(path, ARRAYSIZE(path), L"\\ACVFSR31\\jitter_map.txt"))) {
        const HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
            CloseHandle(file);
        }
    }
    logLine("JITTER_MAP done frames=%u changed_dwords=%u code_refs=%u kept=%u bytes=%llu", m.frames, changed, refTotal, refCount,
        static_cast<unsigned long long>(out.size()));
    MessageBeep(MB_ICONEXCLAMATION);
    return 0;
}

// Render thread, per prepared frame (from recordJitter): snapshot the window around the table.
void jitterMapFrame(float x, float y) {
    static const ULONG_PTR s_rva = [] {
        char text[32];
        if (!readMarker(L"acvfsr_jitter_map.txt", text, sizeof(text))) return ULONG_PTR{0};
        const unsigned long long rva = strtoull(text, nullptr, 16);
        return static_cast<ULONG_PTR>(rva ? rva : kJitterMapDefaultRva);
    }();
    JitterMap& m = g_jitterMap;
    if (!s_rva || m.started || g_jitter.count < kJitterMapStartFrames) return;
    const UINT span = 2 * kJitterMapWindow;
    if (m.snapshots.empty()) {
        m.table = reinterpret_cast<ULONG_PTR>(GetModuleHandleW(nullptr)) + s_rva;
        m.snapshots.resize(static_cast<size_t>(kJitterMapFrames) * span);
        logLine("JITTER_MAP watching table=%p for %u frames", reinterpret_cast<void*>(m.table), kJitterMapFrames);
    }
    BYTE* slot = m.snapshots.data() + static_cast<size_t>(m.frames) * span;
    if (!copyGuarded(slot, reinterpret_cast<const void*>(m.table - kJitterMapWindow), span)) {
        ++m.faulted;
        std::memset(slot, 0, span);
    }
    m.jitter[m.frames][0] = x;
    m.jitter[m.frames][1] = y;
    if (++m.frames < kJitterMapFrames) return;
    m.started = true;
    HANDLE thread = CreateThread(nullptr, 0, jitterMapThread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    else logLine("JITTER_MAP error=thread");
}

// ---------- jitter phases (in-memory code patch, both builds) ----------
// ACValhalla.exe picks its TAA jitter in a switch over a mode (rva 0xaa14c9, found with -JitterMap + tools/exe_code.ps1): mode 2 =
// Halton 8 (what the game runs), 3 = Halton 16, 4 = Halton 32. The mode is copied from a renderer field every frame, so writing it
// does not last; instead entry 2 of the switch's jump table (rva 0xaa15bc, in .text1) is pointed at the case of mode 3 or 4.
// Memory only, nothing on disk. The code and table are checked first, so another game version leaves the feature off. The entry
// goes back when FSR 4 is off or 8 is chosen. If anything else changes the entry the mod stops touching it (anti-tamper?).
constexpr BYTE kJitterSwitchCode[] = {0xe8, 0x12, 0x0b, 0x11, 0x00, 0x48, 0x8b, 0xc8, 0x8b, 0x05, 0x49, 0x6f, 0x5d, 0x05, 0x48,
    0xff, 0xc0, 0x48, 0x03, 0xc0, 0x48, 0x8b, 0x84, 0xc1, 0x08, 0x02, 0x00, 0x00, 0x48, 0x63, 0x48, 0x14, 0x83, 0xf9, 0x0b, 0x0f,
    0x87, 0xa8, 0x00, 0x00, 0x00};
constexpr ULONG_PTR kJitterSwitchRva = 0xaa14c9, kJitterJumpTableRva = 0xaa15bc;
constexpr UINT32 kJitterCases[3] = {0xaa151b, 0xaa1527, 0xaa1533};   // Table entries 2, 3, 4: Halton 8, 16, 32.
constexpr ULONGLONG kJitterCheckMs = 500;

struct JitterControl {
    bool checked = false;
    bool supported = false;
    UINT32* slot = nullptr;              // Jump table entry 2.
    UINT32 cases[3]{};                   // Entries 2, 3, 4 as found.
    UINT32 written = 0;                  // What the entry should hold now.
    ULONGLONG nextCheck = 0;
    ULONGLONG verifyAt = 0;              // Log the phases actually seen a few seconds after a change.
};
JitterControl g_jitterControl;

UINT distinctJitterSeen() {
    const UINT available = (std::min)(g_jitter.count, kJitterHistory);
    float values[kJitterHistory];
    UINT count = 0;
    for (UINT i = 0; i < available; ++i) {
        bool known = false;
        for (UINT k = 0; k < count && !known; ++k) known = std::fabs(values[k] - g_jitter.x[i]) < 1e-5f;
        if (!known) values[count++] = g_jitter.x[i];
    }
    return count;
}

// SEH only: one atomic aligned write into read-only code.
bool writeCode32(UINT32* address, UINT32 value) {
    DWORD old = 0, unused = 0;
    if (!VirtualProtect(address, sizeof(UINT32), PAGE_EXECUTE_READWRITE, &old)) return false;
    bool ok = true;
    __try {
        InterlockedExchange(reinterpret_cast<volatile LONG*>(address), static_cast<LONG>(value));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    VirtualProtect(address, sizeof(UINT32), old, &unused);
    FlushInstructionCache(GetCurrentProcess(), address, sizeof(UINT32));
    return ok;
}

void checkJitterSupport(JitterControl& jc) {
    jc.checked = true;
    const BYTE* exe = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
    ULONG_PTR tableRva = kJitterJumpTableRva;
    bool verifyCode = true;
    char test[32]{};
    // Probe build only: the mock test passes its own table RVA (hex) and has no game code to check.
    if (!kRelease && GetEnvironmentVariableA("ACVFSR_JITTER_TEST", test, sizeof(test)) > 0) {
        tableRva = static_cast<ULONG_PTR>(strtoull(test, nullptr, 16));
        verifyCode = false;
    }
    if (verifyCode) {
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(exe + reinterpret_cast<const IMAGE_DOS_HEADER*>(exe)->e_lfanew);
        BYTE code[sizeof(kJitterSwitchCode)]{};
        if (nt->OptionalHeader.SizeOfImage <= kJitterJumpTableRva + 0x40 || !copyGuarded(code, exe + kJitterSwitchRva, sizeof(code)) ||
            std::memcmp(code, kJitterSwitchCode, sizeof(code)) != 0) {
            logLine("FSR_JITTER supported=0 (unknown game version: jitter phases stay at 8)");
            return;
        }
    }
    UINT32 entries[5]{};
    if (!copyGuarded(entries, exe + tableRva, sizeof(entries))) return;
    if (verifyCode && (entries[2] != kJitterCases[0] || entries[3] != kJitterCases[1] || entries[4] != kJitterCases[2])) {
        logLine("FSR_JITTER supported=0 (jump table differs: jitter phases stay at 8)");
        return;
    }
    jc.slot = reinterpret_cast<UINT32*>(reinterpret_cast<ULONG_PTR>(exe) + tableRva + 2 * sizeof(UINT32));
    std::memcpy(jc.cases, entries + 2, sizeof(jc.cases));
    jc.written = jc.cases[0];
    jc.supported = true;
    logLine("FSR_JITTER supported=1 table=%p", static_cast<const void*>(exe + tableRva));
}

// Polled with the keys: the jump table entry follows the menu while FSR 4 is on, the game's own value otherwise.
void applyJitterPhases() {
    JitterControl& jc = g_jitterControl;
    const ULONGLONG now = GetTickCount64();
    if (jc.verifyAt && now >= jc.verifyAt) {
        jc.verifyAt = 0;
        logLine("FSR_JITTER_SEEN distinct_x=%u over the last %u prepared frames (wanted %u)", distinctJitterSeen(),
            (std::min)(g_jitter.count, kJitterHistory), g_fsrEnabled.load() ? g_up.jitterPhases : 8u);
    }
    if (now < jc.nextCheck) return;
    jc.nextCheck = now + kJitterCheckMs;
    if (!jc.checked) checkJitterSupport(jc);
    if (!jc.supported) return;
    UINT32 current = 0;
    if (!copyGuarded(&current, jc.slot, sizeof(current))) return;
    if (current != jc.written) {
        logLine("FSR_JITTER_REVERTED entry=0x%x expected=0x%x (changed by something else: leaving it alone, phases stay at 8)", current, jc.written);
        jc.supported = false;
        return;
    }
    const UINT phases = g_fsrEnabled.load() ? g_up.jitterPhases : 8u;
    const UINT32 wanted = jc.cases[phases == 32 ? 2 : (phases == 16 ? 1 : 0)];
    if (current == wanted) return;
    if (!writeCode32(jc.slot, wanted)) {
        logLine("FSR_JITTER_PATCH failed error=%lu (phases stay at 8)", GetLastError());
        jc.supported = false;
        return;
    }
    jc.written = wanted;
    jc.verifyAt = now + 3000;
    logLine("FSR_JITTER_PATCH phases=%u entry=0x%x", phases, wanted);
}

// Menu text for the jitter line: N/A when the game version is not the one the feature was made for.
const char* jitterPhasesText(char* buffer, size_t size) {
    if (g_jitterControl.checked && !g_jitterControl.supported) return "N/A";
    snprintf(buffer, size, "%u", g_up.jitterPhases);
    return buffer;
}

// Setting changes, shared by the F-keys and the F2 menu.
void toggleFsr() {
    const bool enabled = !g_fsrEnabled.load();
    g_fsrEnabled.store(enabled);
    if (enabled) { g_up.needReset = true; flashIndicator(); }
    if (enabled) notify(g_up.nativeMode ? "FSR 4 ON - NATIVE AA" : "FSR 4 ON - UPSCALING", 0.2f, 1.0f, 0.3f);
    else notify("FSR 4 OFF - GAME DEFAULT", 1.0f, 0.3f, 0.25f);
    logLine("FSR_TOGGLE enabled=%d", enabled ? 1 : 0);
    MessageBeep(enabled ? MB_ICONASTERISK : MB_ICONHAND);
}

void changeSharpness(float delta) {
    const float next = std::round((g_up.sharpness + delta) * 10.0f) / 10.0f;
    g_up.sharpness = next < 0.0f ? 0.0f : (next > 1.0f ? 1.0f : next);
    notifySharpness();
    saveSettings();
}

void changeJitterPhases(int direction) {
    static const UINT kSteps[] = {8, 16, 32};
    UINT i = g_up.jitterPhases == 32 ? 2 : (g_up.jitterPhases == 16 ? 1 : 0);
    i = direction < 0 ? (i + 2) % 3 : (i + 1) % 3;
    g_up.jitterPhases = kSteps[i];
    char text[40];
    if (g_jitterControl.checked && !g_jitterControl.supported) snprintf(text, sizeof(text), "JITTER PHASES N/A - GAME VERSION");
    else snprintf(text, sizeof(text), "JITTER PHASES %u%s", g_up.jitterPhases, g_up.jitterPhases == 8 ? " - GAME DEFAULT" : "");
    notify(text, 1.0f, 1.0f, 1.0f);
    logLine("FSR_JITTER_PHASES %u", g_up.jitterPhases);
    g_jitterControl.nextCheck = 0;   // Apply now.
    saveSettings();
}

void toggleSkip() {
    g_up.skipStock = !g_up.skipStock;
    if (g_up.nativeMode) notify(g_up.skipStock ? "SKIP STOCK AA ON - UPSCALE ONLY" : "SKIP STOCK AA OFF - UPSCALE ONLY", 1.0f, 1.0f, 1.0f);
    else notify(g_up.skipStock ? "SKIP STOCK AA ON - FASTER" : "SKIP STOCK AA OFF", g_up.skipStock ? 0.2f : 1.0f,
        g_up.skipStock ? 1.0f : 0.6f, g_up.skipStock ? 0.3f : 0.2f);
    logLine("FSR_SKIP_STOCK %d", g_up.skipStock ? 1 : 0);
    MessageBeep(g_up.skipStock ? MB_ICONASTERISK : MB_ICONHAND);
    saveSettings();
}

void toggleIndicator() {
    g_up.indicatorAlways = !g_up.indicatorAlways;
    notify(g_up.indicatorAlways ? "INDICATOR ALWAYS ON" : "INDICATOR ONLY ON CHANGES", 1.0f, 1.0f, 1.0f);
    logLine("FSR_INDICATOR always=%d", g_up.indicatorAlways ? 1 : 0);
    MessageBeep(MB_ICONASTERISK);
    saveSettings();
}

// F2 menu entries (in order). The mode line is information only and is skipped when moving the selection.
enum MenuItem : UINT { kMenuFsr = 0, kMenuSharpness, kMenuJitter, kMenuSkip, kMenuIndicator, kMenuMode, kMenuClose, kMenuItemCount };

void buildMenuLines(char lines[][kTextMaxChars + 1], float colors[][3], UINT* count) {
    static const float kWhite[3] = {0.9f, 0.9f, 0.9f}, kYellow[3] = {1.0f, 0.85f, 0.2f}, kGray[3] = {0.55f, 0.55f, 0.55f},
        kCyan[3] = {0.3f, 0.9f, 1.0f};
    UINT n = 0;
    auto add = [&](const float* rgb, const char* format, auto... args) {
        snprintf(lines[n], kTextMaxChars + 1, format, args...);
        std::memcpy(colors[n], rgb, sizeof(float) * 3);
        ++n;
    };
    add(kCyan, "FSR 4 SETTINGS      F2 CLOSE");
    for (UINT item = 0; item < kMenuItemCount; ++item) {
        const bool selected = g_up.menuSelection == item;
        const char mark = selected ? '>' : ' ';
        const float* rgb = selected ? kYellow : kWhite;
        switch (item) {
        case kMenuFsr: add(rgb, "%c FSR 4          %s", mark, g_fsrEnabled.load() ? "ON" : "OFF"); break;
        case kMenuSharpness: add(rgb, "%c SHARPNESS      %.1f", mark, g_up.sharpness); break;
        case kMenuJitter: { char phases[8]; add(rgb, "%c JITTER PHASES  %s", mark, jitterPhasesText(phases, sizeof(phases))); break; }
        case kMenuSkip: add(rgb, "%c SKIP STOCK AA  %s", mark, g_up.skipStock ? "ON" : "OFF"); break;
        case kMenuIndicator: add(rgb, "%c INDICATOR      %s", mark, g_up.indicatorAlways ? "ALWAYS" : "ON CHANGES"); break;
        case kMenuMode: add(kGray, "  MODE           %s", g_up.nativeMode ? "NATIVE AA" : "UPSCALE"); break;
        case kMenuClose: add(rgb, "%c CLOSE", mark); break;
        default: break;
        }
    }
    add(kGray, "UP/DOWN SELECT  LEFT/RIGHT SET");
    *count = n;
}

// Keys while the F2 menu is open: Up/Down select, Left/Right change, Enter toggles / activates.
void pollMenuKeys() {
    if (pollKeyEdge(VK_F2, g_f2Down)) {
        g_up.menuOpen = !g_up.menuOpen;
        logLine("FSR_MENU open=%d", g_up.menuOpen ? 1 : 0);
        MessageBeep(MB_OK);
    }
    // Edges are tracked even while closed so a held key doesn't fire when the menu opens.
    const bool up = pollKeyEdge(VK_UP, g_upDown), down = pollKeyEdge(VK_DOWN, g_downDown);
    const bool left = pollKeyEdge(VK_LEFT, g_leftDown), right = pollKeyEdge(VK_RIGHT, g_rightDown);
    const bool enter = pollKeyEdge(VK_RETURN, g_enterDown);
    if (!g_up.menuOpen) return;
    if (up || down) {
        do {
            g_up.menuSelection = (g_up.menuSelection + (up ? kMenuItemCount - 1 : 1)) % kMenuItemCount;
        } while (g_up.menuSelection == kMenuMode);
    }
    if (!(left || right || enter)) return;
    switch (g_up.menuSelection) {
    case kMenuFsr: toggleFsr(); break;
    case kMenuSharpness: changeSharpness(left ? -0.1f : 0.1f); break;
    case kMenuJitter: changeJitterPhases(left ? -1 : 1); break;
    case kMenuSkip: toggleSkip(); break;
    case kMenuIndicator: toggleIndicator(); break;
    case kMenuClose: if (enter) { g_up.menuOpen = false; logLine("FSR_MENU open=0"); } break;
    default: break;
    }
}

void pollFsrKey() {
    loadSettings();
    watchFsrActivity();
    pollMenuKeys();
    applyJitterPhases();
    if (!kRelease) pollJitterScan();
    if (pollKeyEdge(VK_F7, g_f7Down)) toggleFsr();
    // F8 / F9: FSR 4 sharpening -/+ 0.1 (release build; the probe build uses these keys for its dump / trace).
    if (kRelease) {
        if (pollKeyEdge(VK_F8, g_f8SharpDown)) changeSharpness(-0.1f);
        if (pollKeyEdge(VK_F9, g_f9SharpDown)) changeSharpness(0.1f);
    }
    if (!kRelease && pollKeyEdge(VK_F6, g_f6Down)) {
        bool expected = false;
        if (g_fsrEnabled.load() && !g_up.broken && g_abBusy.compare_exchange_strong(expected, true)) {
            g_abArmed.store(true);
            logLine("FSR_AB armed");
            MessageBeep(MB_ICONASTERISK);
        }
    }
    if (pollKeyEdge(VK_F3, g_f3Down)) toggleIndicator();
    if (pollKeyEdge(VK_F4, g_f4Down)) toggleSkip();
    if (!kRelease && pollKeyEdge(VK_F5, g_f5Down)) {
        const bool on = !g_fsrDebugView.load();
        g_fsrDebugView.store(on);
        logLine("FSR_DEBUG_VIEW %d", on ? 1 : 0);
        MessageBeep(MB_ICONASTERISK);
    }
}

} // namespace

extern "C" void* WINAPI ResolveD3D12Export(UINT ordinalIndex) {
    return lookupExport(ordinalIndex);
}

extern "C" HRESULT WINAPI D3D12CreateDevice(IUnknown* adapter,
    D3D_FEATURE_LEVEL minimumFeatureLevel, REFIID riid, void** deviceOut) {
    auto original = reinterpret_cast<D3D12CreateDeviceFn>(lookupExport(2));
    if (!original) return E_NOINTERFACE;
    const HRESULT hr = original(adapter, minimumFeatureLevel, riid, deviceOut);
    static std::atomic<bool> s_announced{false};
    if (!s_announced.exchange(true)) {
        logLine("ACV_FSR4 %s build loaded. Keys: F2 settings menu (arrows + Enter), F7 FSR 4 on/off, F4 skip stock TAA+FSR 1 on/off, F8/F9 sharpness -/+ "
            "(release), F3 indicator always on/off. "
            "Game FSR on = upscale mode (green square), game FSR off = native AA mode (cyan square).", kRelease ? "release" : "probe");
    }
    logLine("D3D12CreateDevice adapter=%p minFeatureLevel=0x%X riid=%08X-%04X-%04X hr=0x%08X",
        adapter, static_cast<unsigned>(minimumFeatureLevel), riid.Data1, riid.Data2, riid.Data3,
        static_cast<unsigned>(hr));
    if (SUCCEEDED(hr) && deviceOut && *deviceOut) {
        // D3D12CreateDevice can hand back a newer interface (Valhalla asks
        // for ID3D12Device5). COM implementations may expose that interface
        // with a different vtable from ID3D12Device, so patch both pointers.
        auto* requestedDevice = reinterpret_cast<ID3D12Device*>(*deviceOut);
        const size_t requestedLength = deviceVtableLength(riid);
        const bool requestedPatched = patchDevice(requestedDevice,
            requestedLength ? requestedLength : 47);
        logLine("D3D12 requested-interface hook %s device=%p",
            requestedPatched ? "installed" : "failed", requestedDevice);

        IUnknown* unknown = static_cast<IUnknown*>(*deviceOut);
        ID3D12Device* baseDevice = nullptr;
        if (SUCCEEDED(unknown->QueryInterface(__uuidof(ID3D12Device),
            reinterpret_cast<void**>(&baseDevice)))) {
            const bool patched = patchDevice(baseDevice, 47);
            logLine("D3D12 device hook %s device=%p", patched ? "installed" : "failed", baseDevice);
            if (patched) installTraceHooks(baseDevice);
            scheduleFfxTest(baseDevice);
            baseDevice->Release();
        } else {
            logLine("D3D12 device hook skipped: requested interface did not expose ID3D12Device.");
        }
    }
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}
