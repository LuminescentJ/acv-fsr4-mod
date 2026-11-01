#include "factory_proxy.hpp"
#include "proxy_log.hpp"

#include <cstdio>

FactoryProxy::FactoryProxy(IDXGIFactory7* realFactory) noexcept : realFactory_(realFactory) {
    if (realFactory_) realFactory_->AddRef();
}
HRESULT STDMETHODCALLTYPE FactoryProxy::QueryInterface(REFIID riid, void** object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
        riid == __uuidof(IDXGIFactory) || riid == __uuidof(IDXGIFactory1) ||
        riid == __uuidof(IDXGIFactory2) || riid == __uuidof(IDXGIFactory3) ||
        riid == __uuidof(IDXGIFactory4) || riid == __uuidof(IDXGIFactory5)) {
        *object = static_cast<IDXGIFactory7*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == __uuidof(IDXGIFactory6) || riid == __uuidof(IDXGIFactory7)) {
        *object = static_cast<IDXGIFactory7*>(this);
        AddRef();
        return S_OK;
    }
    wchar_t guidText[64]{};
    StringFromGUID2(riid, guidText, ARRAYSIZE(guidText));
    char line[128]{};
    std::snprintf(line, sizeof(line), "Factory QueryInterface passed through (%.60ls).", guidText);
    writeProxyLog(line);
    return realFactory_->QueryInterface(riid, object);
}
ULONG STDMETHODCALLTYPE FactoryProxy::AddRef() { return ++references_; }
ULONG STDMETHODCALLTYPE FactoryProxy::Release() {
    const ULONG remaining = --references_;
    if (remaining == 0) { realFactory_->Release(); delete this; }
    return remaining;
}
HRESULT STDMETHODCALLTYPE FactoryProxy::SetPrivateData(REFGUID n, UINT s, const void* d) { return realFactory_->SetPrivateData(n,s,d); }
HRESULT STDMETHODCALLTYPE FactoryProxy::SetPrivateDataInterface(REFGUID n, const IUnknown* u) { return realFactory_->SetPrivateDataInterface(n,u); }
HRESULT STDMETHODCALLTYPE FactoryProxy::GetPrivateData(REFGUID n, UINT* s, void* d) { return realFactory_->GetPrivateData(n,s,d); }
HRESULT STDMETHODCALLTYPE FactoryProxy::GetParent(REFIID i, void** p) { return realFactory_->GetParent(i,p); }
HRESULT STDMETHODCALLTYPE FactoryProxy::EnumAdapters(UINT a, IDXGIAdapter** r) { return realFactory_->EnumAdapters(a,r); }
HRESULT STDMETHODCALLTYPE FactoryProxy::MakeWindowAssociation(HWND w, UINT f) { return realFactory_->MakeWindowAssociation(w,f); }
HRESULT STDMETHODCALLTYPE FactoryProxy::GetWindowAssociation(HWND* w) { return realFactory_->GetWindowAssociation(w); }
HRESULT STDMETHODCALLTYPE FactoryProxy::CreateSwapChain(IUnknown* d, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** sc) {
    if (desc) {
        char line[160]{};
        std::snprintf(line,sizeof(line),"CreateSwapChain requested: %ux%u buffers=%u format=%u",desc->BufferDesc.Width,desc->BufferDesc.Height,desc->BufferCount,static_cast<unsigned>(desc->BufferDesc.Format));
        writeProxyLog(line);
    }
    const HRESULT hr = realFactory_->CreateSwapChain(d,desc,sc);
    writeProxyLog(SUCCEEDED(hr) ? "CreateSwapChain succeeded." : "CreateSwapChain failed.");
    return hr;
}
HRESULT STDMETHODCALLTYPE FactoryProxy::CreateSoftwareAdapter(HMODULE m, IDXGIAdapter** a) { return realFactory_->CreateSoftwareAdapter(m,a); }
HRESULT STDMETHODCALLTYPE FactoryProxy::EnumAdapters1(UINT a, IDXGIAdapter1** r) { return realFactory_->EnumAdapters1(a,r); }
BOOL STDMETHODCALLTYPE FactoryProxy::IsCurrent() { return realFactory_->IsCurrent(); }
BOOL STDMETHODCALLTYPE FactoryProxy::IsWindowedStereoEnabled() { return realFactory_->IsWindowedStereoEnabled(); }
HRESULT STDMETHODCALLTYPE FactoryProxy::CreateSwapChainForHwnd(IUnknown* d, HWND w, const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fd, IDXGIOutput* o, IDXGISwapChain1** sc) {
    logSwapChain("CreateSwapChainForHwnd",desc);
    const HRESULT hr = realFactory_->CreateSwapChainForHwnd(d,w,desc,fd,o,sc);
    writeProxyLog(SUCCEEDED(hr) ? "CreateSwapChainForHwnd succeeded." : "CreateSwapChainForHwnd failed.");
    return hr;
}
HRESULT STDMETHODCALLTYPE FactoryProxy::CreateSwapChainForCoreWindow(IUnknown* d, IUnknown* w, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* o, IDXGISwapChain1** sc) {
    logSwapChain("CreateSwapChainForCoreWindow",desc);
    const HRESULT hr = realFactory_->CreateSwapChainForCoreWindow(d,w,desc,o,sc);
    writeProxyLog(SUCCEEDED(hr) ? "CreateSwapChainForCoreWindow succeeded." : "CreateSwapChainForCoreWindow failed.");
    return hr;
}
HRESULT STDMETHODCALLTYPE FactoryProxy::GetSharedResourceAdapterLuid(HANDLE r, LUID* l) { return realFactory_->GetSharedResourceAdapterLuid(r,l); }
HRESULT STDMETHODCALLTYPE FactoryProxy::RegisterStereoStatusWindow(HWND w, UINT m, DWORD* c) { return realFactory_->RegisterStereoStatusWindow(w,m,c); }
HRESULT STDMETHODCALLTYPE FactoryProxy::RegisterStereoStatusEvent(HANDLE e, DWORD* c) { return realFactory_->RegisterStereoStatusEvent(e,c); }
void STDMETHODCALLTYPE FactoryProxy::UnregisterStereoStatus(DWORD c) { realFactory_->UnregisterStereoStatus(c); }
HRESULT STDMETHODCALLTYPE FactoryProxy::RegisterOcclusionStatusWindow(HWND w, UINT m, DWORD* c) { return realFactory_->RegisterOcclusionStatusWindow(w,m,c); }
HRESULT STDMETHODCALLTYPE FactoryProxy::RegisterOcclusionStatusEvent(HANDLE e, DWORD* c) { return realFactory_->RegisterOcclusionStatusEvent(e,c); }
void STDMETHODCALLTYPE FactoryProxy::UnregisterOcclusionStatus(DWORD c) { realFactory_->UnregisterOcclusionStatus(c); }
HRESULT STDMETHODCALLTYPE FactoryProxy::CreateSwapChainForComposition(IUnknown* d, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* o, IDXGISwapChain1** sc) {
    logSwapChain("CreateSwapChainForComposition",desc);
    const HRESULT hr = realFactory_->CreateSwapChainForComposition(d,desc,o,sc);
    writeProxyLog(SUCCEEDED(hr) ? "CreateSwapChainForComposition succeeded." : "CreateSwapChainForComposition failed.");
    return hr;
}
UINT STDMETHODCALLTYPE FactoryProxy::GetCreationFlags() { return realFactory_->GetCreationFlags(); }
HRESULT STDMETHODCALLTYPE FactoryProxy::EnumAdapterByLuid(LUID luid, REFIID riid, void** adapter) { return realFactory_->EnumAdapterByLuid(luid,riid,adapter); }
HRESULT STDMETHODCALLTYPE FactoryProxy::EnumWarpAdapter(REFIID riid, void** adapter) { return realFactory_->EnumWarpAdapter(riid,adapter); }
HRESULT STDMETHODCALLTYPE FactoryProxy::CheckFeatureSupport(DXGI_FEATURE feature, void* data, UINT size) { return realFactory_->CheckFeatureSupport(feature,data,size); }
HRESULT STDMETHODCALLTYPE FactoryProxy::EnumAdapterByGpuPreference(UINT adapter, DXGI_GPU_PREFERENCE preference, REFIID riid, void** result) { return realFactory_->EnumAdapterByGpuPreference(adapter,preference,riid,result); }
HRESULT STDMETHODCALLTYPE FactoryProxy::RegisterAdaptersChangedEvent(HANDLE eventHandle, DWORD* cookie) { return realFactory_->RegisterAdaptersChangedEvent(eventHandle,cookie); }
HRESULT STDMETHODCALLTYPE FactoryProxy::UnregisterAdaptersChangedEvent(DWORD cookie) { return realFactory_->UnregisterAdaptersChangedEvent(cookie); }
void FactoryProxy::logSwapChain(const char* method, const DXGI_SWAP_CHAIN_DESC1* desc) const {
    if (!desc) { writeProxyLog(method); return; }
    char line[160]{};
    std::snprintf(line,sizeof(line),"%s requested: %ux%u buffers=%u format=%u samples=%u",method,desc->Width,desc->Height,desc->BufferCount,static_cast<unsigned>(desc->Format),desc->SampleDesc.Count);
    writeProxyLog(line);
}
