// Loads build\dxgi.dll (the proxy) like the game would and exercises the hooked paths.
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d11.h>
#include <cstdio>
#pragma comment(lib, "d3d11.lib")

static void DumpOutputs(IDXGIAdapter1* a) {
    DXGI_ADAPTER_DESC1 ad{}; a->GetDesc1(&ad);
    printf("  adapter: %ls\n", ad.Description);
    for (UINT o = 0;; ++o) {
        IDXGIOutput* out = nullptr;
        if (FAILED(a->EnumOutputs(o, &out))) break;
        DXGI_OUTPUT_DESC d{}; out->GetDesc(&d);
        printf("    output %u: %ls (%ld,%ld)\n", o, d.DeviceName, d.DesktopCoordinates.left, d.DesktopCoordinates.top);
        out->Release();
    }
}

int wmain(int argc, wchar_t** argv) {
    const wchar_t* dll = argc > 1 ? argv[1] : L"dxgi.dll";
    printf("loading %ls\n", dll); fflush(stdout);
    HMODULE m = LoadLibraryW(dll);
    printf("module %p (err %lu)\n", m, GetLastError()); fflush(stdout);
    if (!m) return 1;
    auto create = (HRESULT(WINAPI*)(REFIID, void**))GetProcAddress(m, "CreateDXGIFactory1");
    IDXGIFactory1* f = nullptr;
    HRESULT hr = create(__uuidof(IDXGIFactory1), (void**)&f);
    printf("CreateDXGIFactory1 hr=%08lx\n", hr); fflush(stdout);
    if (FAILED(hr)) return 1;
    printf("EnumAdapters1 order:\n");
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* a = nullptr;
        if (FAILED(f->EnumAdapters1(i, &a))) break;
        DumpOutputs(a); a->Release();
    }
    IDXGIFactory6* f6 = nullptr;
    if (SUCCEEDED(f->QueryInterface(IID_PPV_ARGS(&f6)))) {
        printf("EnumAdapterByGpuPreference(HIGH_PERFORMANCE) order:\n");
        for (UINT i = 0;; ++i) {
            IDXGIAdapter1* a = nullptr;
            if (FAILED(f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)))) break;
            DumpOutputs(a); a->Release();
        }
        f6->Release();
    }
    // d3d11 device creation (d3d11 binds to whichever dxgi is loaded)
    ID3D11Device* dev = nullptr;
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, nullptr);
    printf("D3D11CreateDevice hr=%08lx\n", hr);
    if (dev) dev->Release();
    f->Release();
    printf("ok\n");
    return 0;
}
