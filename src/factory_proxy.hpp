#pragma once

#include <dxgi1_6.h>
#include <atomic>

class FactoryProxy final : public IDXGIFactory7 {
public:
    explicit FactoryProxy(IDXGIFactory7* realFactory) noexcept;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID name, UINT size, const void* data) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID name, const IUnknown* unknown) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID name, UINT* size, void* data) override;
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** parent) override;
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT adapter, IDXGIAdapter** result) override;
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND window, UINT flags) override;
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* window) override;
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain) override;
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE module, IDXGIAdapter** adapter) override;
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT adapter, IDXGIAdapter1** result) override;
    BOOL STDMETHODCALLTYPE IsCurrent() override;
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override;
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* device, HWND window, const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc, IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapChain) override;
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown* device, IUnknown* window, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapChain) override;
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE resource, LUID* luid) override;
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND window, UINT message, DWORD* cookie) override;
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE eventHandle, DWORD* cookie) override;
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD cookie) override;
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND window, UINT message, DWORD* cookie) override;
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE eventHandle, DWORD* cookie) override;
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD cookie) override;
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapChain) override;
    UINT STDMETHODCALLTYPE GetCreationFlags() override;
    HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID adapterLuid, REFIID riid, void** adapter) override;
    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID riid, void** adapter) override;
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE feature, void* featureSupportData, UINT featureSupportDataSize) override;
    HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(UINT adapter, DXGI_GPU_PREFERENCE gpuPreference, REFIID riid, void** ppvAdapter) override;
    HRESULT STDMETHODCALLTYPE RegisterAdaptersChangedEvent(HANDLE event, DWORD* cookie) override;
    HRESULT STDMETHODCALLTYPE UnregisterAdaptersChangedEvent(DWORD cookie) override;
private:
    ~FactoryProxy() = default;
    void logSwapChain(const char* method, const DXGI_SWAP_CHAIN_DESC1* desc) const;
    std::atomic<ULONG> references_{1};
    IDXGIFactory7* realFactory_ = nullptr;
};
