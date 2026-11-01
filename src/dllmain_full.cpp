#include <windows.h>
#include <strsafe.h>
#include <dxgi1_3.h>
#include "proxy_log.hpp"
#include <cstdio>

namespace {
INIT_ONCE g_dxgiInitOnce = INIT_ONCE_STATIC_INIT;
HMODULE g_systemDxgi = nullptr;

constexpr const char* kSystemExports[] = {
    "ApplyCompatResolutionQuirking", "CompatString", "CompatValue", "DXGIDumpJournal",
    "PIXBeginCapture", "PIXEndCapture", "PIXGetCaptureState", "SetAppCompatStringPointer",
    "UpdateHMDEmulationStatus", "CreateDXGIFactory", "CreateDXGIFactory1", "CreateDXGIFactory2",
    "DXGID3D10CreateDevice", "DXGID3D10CreateLayeredDevice", "DXGID3D10GetLayeredDeviceSize",
    "DXGID3D10RegisterLayers", "DXGIDeclareAdapterRemovalSupport", "DXGIDisableVBlankVirtualization",
    "DXGIGetDebugInterface1", "DXGIReportAdapterConfiguration",
};

BOOL CALLBACK loadSystemDxgi(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t systemDxgiPath[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(systemDxgiPath, MAX_PATH);
    if (length == 0 || length + 10 >= MAX_PATH) return TRUE;
    lstrcatW(systemDxgiPath, L"\\dxgi.dll");
    // The absolute path is essential: this proxy itself is also named dxgi.dll.
    g_systemDxgi = LoadLibraryW(systemDxgiPath);
    return TRUE;
}

HMODULE systemDxgi() {
    InitOnceExecuteOnce(&g_dxgiInitOnce, loadSystemDxgi, nullptr, nullptr);
    return g_systemDxgi;
}

}  // namespace

void writeProxyLog(const char* message) {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return;
    lstrcatW(path, L"\\ACVFSR31");
    CreateDirectoryW(path, nullptr);
    lstrcatW(path, L"\\proxy.log");

    const HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char line[512]{};
    const size_t messageLength = strnlen_s(message, sizeof(line) - 3);
    CopyMemory(line, message, messageLength);
    line[messageLength] = '\r';
    line[messageLength + 1] = '\n';
    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(messageLength + 2), &written, nullptr);
    CloseHandle(file);
}

extern "C" void* WINAPI ResolveDxgiExport(UINT ordinalIndex) {
    if (ordinalIndex >= ARRAYSIZE(kSystemExports)) return nullptr;
    const HMODULE module = systemDxgi();
    return module ? reinterpret_cast<void*>(GetProcAddress(module, kSystemExports[ordinalIndex])) : nullptr;
}

using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
using CreateFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
using GetDebugInterfaceFn = HRESULT(WINAPI*)(UINT, REFIID, void**);

namespace {
void logFactoryResult(const char* name, HRESULT result) {
    char line[128]{};
    std::snprintf(line, sizeof(line), "%s forwarded without COM wrapping; HRESULT=0x%08X.",
        name, static_cast<unsigned int>(result));
    writeProxyLog(line);
}
}  // namespace

extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** factory) {
    const auto fn = reinterpret_cast<CreateFactoryFn>(ResolveDxgiExport(9));
    if (!fn) return E_NOINTERFACE;
    const HRESULT result = fn(riid, factory);
    logFactoryResult("CreateDXGIFactory", result);
    return result;
}
extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** factory) {
    const auto fn = reinterpret_cast<CreateFactoryFn>(ResolveDxgiExport(10));
    if (!fn) return E_NOINTERFACE;
    const HRESULT result = fn(riid, factory);
    logFactoryResult("CreateDXGIFactory1", result);
    return result;
}
extern "C" HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** factory) {
    const auto fn = reinterpret_cast<CreateFactory2Fn>(ResolveDxgiExport(11));
    if (!fn) return E_NOINTERFACE;
    const HRESULT result = fn(flags, riid, factory);
    logFactoryResult("CreateDXGIFactory2", result);
    return result;
}
extern "C" HRESULT WINAPI DXGIGetDebugInterface1(UINT flags, REFIID riid, void** debug) {
    const auto fn = reinterpret_cast<GetDebugInterfaceFn>(ResolveDxgiExport(18));
    return fn ? fn(flags, riid, debug) : E_NOINTERFACE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}
