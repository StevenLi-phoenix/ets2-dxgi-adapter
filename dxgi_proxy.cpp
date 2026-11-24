// dxgi.dll proxy for Euro Truck Simulator 2.
// Makes a chosen monitor appear as DXGI output #0 (and its GPU as adapter #0),
// so the game's default display path (r_output -1 / 0) lands on that monitor
// without changing the Windows primary display.
//
// Config: ets2_monitor_adapter.ini next to this DLL:
//   [adapter]
//   monitor=auto         ; auto = first non-primary monitor, or a substring of the monitor's
//                        ; device id (e.g. AOCB305), or a GDI name like \\.\DISPLAY5
//   reorder_adapters=1
//   reorder_outputs=1
//   hook_win32=1
//   log=0
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_6.h>
#include <cstdio>
#include <cwchar>

// NOTE: the Windows app-compat engine (apphelp/AcGenral) calls dxgi's compat exports
// (SetAppCompatStringPointer, CompatValue, ...) during the load notification, i.e.
// before our DllMain and before the CRT is initialised. Everything reachable from the
// export stubs must therefore work on zero-initialised statics: SRWLOCK / INIT_ONCE
// only, no C++ static constructors, and no CRT calls until g_ready is set.

extern "C" FARPROC g_real[20];
FARPROC g_real[20];

static const char* kExportNames[20] = {
    "ApplyCompatResolutionQuirking", "CompatString", "CompatValue", "DXGIDumpJournal",
    "PIXBeginCapture", "PIXEndCapture", "PIXGetCaptureState", "SetAppCompatStringPointer",
    "UpdateHMDEmulationStatus", "CreateDXGIFactory", "CreateDXGIFactory1", "CreateDXGIFactory2",
    "DXGID3D10CreateDevice", "DXGID3D10CreateLayeredDevice", "DXGID3D10GetLayeredDeviceSize",
    "DXGID3D10RegisterLayers", "DXGIDeclareAdapterRemovalSupport", "DXGIDisableVBlankVirtualization",
    "DXGIGetDebugInterface1", "DXGIReportAdapterConfiguration"};

static HMODULE g_self, g_sys;
static wchar_t g_dir[MAX_PATH];
static wchar_t g_monitor[128] = L"auto";
static bool g_reorderAdapters = true, g_reorderOutputs = true, g_hookWin32 = true, g_log = false;
static FILE* g_logf;
static volatile LONG g_ready;          // set at the end of DLL_PROCESS_ATTACH (CRT is up)
static SRWLOCK g_logLock = SRWLOCK_INIT;
static SRWLOCK g_hookLock = SRWLOCK_INIT;

struct SrwGuard {
    SRWLOCK* l;
    explicit SrwGuard(SRWLOCK* x) : l(x) { AcquireSRWLockExclusive(l); }
    ~SrwGuard() { ReleaseSRWLockExclusive(l); }
};

static void Log(const char* fmt, ...) {
    if (!g_log || !g_ready) return;
    SrwGuard lk(&g_logLock);
    if (!g_logf) {
        wchar_t p[MAX_PATH]; swprintf_s(p, L"%sets2_monitor_adapter.log", g_dir);
        g_logf = _wfsopen(p, L"w", _SH_DENYNO);  // shareable, so the log can be read while the game runs
        if (!g_logf) return;
    }
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_logf, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_logf, fmt, a); va_end(a);
    fputc('\n', g_logf); fflush(g_logf);
}

static void LoadConfig() {
    wchar_t ini[MAX_PATH]; swprintf_s(ini, L"%sets2_monitor_adapter.ini", g_dir);
    GetPrivateProfileStringW(L"adapter", L"monitor", L"auto", g_monitor, 128, ini);
    g_reorderAdapters = GetPrivateProfileIntW(L"adapter", L"reorder_adapters", 1, ini) != 0;
    g_reorderOutputs = GetPrivateProfileIntW(L"adapter", L"reorder_outputs", 1, ini) != 0;
    g_hookWin32 = GetPrivateProfileIntW(L"adapter", L"hook_win32", 1, ini) != 0;
    g_log = GetPrivateProfileIntW(L"adapter", L"log", 0, ini) != 0;
}

// The monitor spec actually matched against. For monitor=auto (or empty) this resolves,
// once, to the GDI name of the first desktop-attached display that is not the primary.
static wchar_t g_spec[128];
static INIT_ONCE g_specOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK ResolveSpec(PINIT_ONCE, PVOID, PVOID*) {
    if (g_monitor[0] && _wcsicmp(g_monitor, L"auto") != 0) {
        wcscpy_s(g_spec, g_monitor);
        return TRUE;
    }
    wcscpy_s(g_spec, L"\x01");  // matches nothing if there is no secondary display
    DISPLAY_DEVICEW dd{}; dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        if ((dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) && !(dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)) {
            wcscpy_s(g_spec, dd.DeviceName);
            break;
        }
        dd.cb = sizeof(dd);
    }
    Log("monitor=auto resolved to %ls", g_spec[0] == 1 ? L"(none)" : g_spec);
    return TRUE;
}

static const wchar_t* TargetSpec() {
    InitOnceExecuteOnce(&g_specOnce, ResolveSpec, nullptr, nullptr);
    return g_spec;
}

// ---- vtable helpers -------------------------------------------------------
static void** VT(void* obj) { return *reinterpret_cast<void***>(obj); }
static bool Patch(void** slot, void* fn, void** orig) {
    if (*slot == fn) return false;
    DWORD old; if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    if (orig && !*orig) *orig = *slot;
    *slot = fn;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

// Originals
using PFN_EnumAdapters = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, UINT, IDXGIAdapter**);
using PFN_EnumAdapters1 = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory1*, UINT, IDXGIAdapter1**);
using PFN_EnumByPref = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory6*, UINT, DXGI_GPU_PREFERENCE, REFIID, void**);
using PFN_EnumByLuid = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory4*, LUID, REFIID, void**);
using PFN_EnumOutputs = HRESULT(STDMETHODCALLTYPE*)(IDXGIAdapter*, UINT, IDXGIOutput**);
static PFN_EnumAdapters o_EnumAdapters;
static PFN_EnumAdapters1 o_EnumAdapters1;
static PFN_EnumByPref o_EnumByPref;
static PFN_EnumByLuid o_EnumByLuid;
static PFN_EnumOutputs o_EnumOutputs;

// ---- target detection ------------------------------------------------------
// True if the display with this GDI name (\\.\DISPLAYn) is the configured target:
// exact GDI name match, or the spec is a substring of the monitor's device interface id.
static bool NameMatchesTarget(const wchar_t* gdiName) {
    const wchar_t* spec = TargetSpec();
    if (_wcsicmp(gdiName, spec) == 0) return true;
    wchar_t want[128]; wcscpy_s(want, spec); _wcsupr_s(want);
    DISPLAY_DEVICEW dd{}; dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(gdiName, i, &dd, EDD_GET_DEVICE_INTERFACE_NAME); ++i) {
        wchar_t id[256]; wcscpy_s(id, dd.DeviceID); _wcsupr_s(id);
        if (wcsstr(id, want)) return true;
        dd.cb = sizeof(dd);
    }
    return false;
}

static bool OutputIsTarget(IDXGIOutput* out, wchar_t* nameOut = nullptr) {
    DXGI_OUTPUT_DESC d{};
    if (FAILED(out->GetDesc(&d))) return false;
    if (nameOut) wcscpy_s(nameOut, 32, d.DeviceName);
    return NameMatchesTarget(d.DeviceName);
}

// Index of the target output on this adapter, or -1.
static int TargetOutputIndex(IDXGIAdapter* ad) {
    for (UINT i = 0;; ++i) {
        IDXGIOutput* o = nullptr;
        if (FAILED(o_EnumOutputs(ad, i, &o))) return -1;
        bool hit = OutputIsTarget(o);
        o->Release();
        if (hit) return (int)i;
    }
}

static void HookAdapter(void* ad);

// Map a requested (virtual) adapter index to the real one.
static UINT MapAdapterIndex(IDXGIFactory* f, UINT idx) {
    if (!g_reorderAdapters) return idx;
    int owner = -1;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter* a = nullptr;
        if (FAILED(o_EnumAdapters(f, i, &a))) break;
        HookAdapter(a);
        int t = TargetOutputIndex(a);
        a->Release();
        if (t >= 0) { owner = (int)i; break; }
    }
    if (owner <= 0) return idx;            // not found or already first
    if (idx == 0) return (UINT)owner;      // target owner moves to front
    if (idx <= (UINT)owner) return idx - 1; // shift the ones before it
    return idx;
}

static void DescribeAdapter(IDXGIAdapter* a, char* buf, size_t n) {
    DXGI_ADAPTER_DESC d{}; a->GetDesc(&d);
    snprintf(buf, n, "%ls", d.Description);
}

// ---- hooks -----------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE H_EnumOutputs(IDXGIAdapter* self, UINT idx, IDXGIOutput** out) {
    UINT real = idx;
    int t = g_reorderOutputs ? TargetOutputIndex(self) : -1;
    if (t > 0) { if (idx == 0) real = (UINT)t; else if (idx <= (UINT)t) real = idx - 1; }
    HRESULT hr = o_EnumOutputs(self, real, out);
    if (g_log) {
        char an[128]; DescribeAdapter(self, an, sizeof an);
        wchar_t nm[32] = L"-";
        if (SUCCEEDED(hr) && out && *out) OutputIsTarget(*out, nm);
        Log("EnumOutputs(%s, %u) -> real %u hr=%08lx %ls", an, idx, real, hr, nm);
    }
    return hr;
}

static void HookAdapter(void* ad) {
    SrwGuard lk(&g_hookLock);
    if (Patch(&VT(ad)[7], (void*)&H_EnumOutputs, (void**)&o_EnumOutputs)) Log("hooked adapter vtable %p", VT(ad));
}

static HRESULT STDMETHODCALLTYPE H_EnumAdapters(IDXGIFactory* self, UINT idx, IDXGIAdapter** out) {
    UINT real = MapAdapterIndex(self, idx);
    HRESULT hr = o_EnumAdapters(self, real, out);
    if (SUCCEEDED(hr) && out && *out) {
        HookAdapter(*out);
        char an[128]; DescribeAdapter(*out, an, sizeof an);
        Log("EnumAdapters(%u) -> real %u %s", idx, real, an);
    } else Log("EnumAdapters(%u) -> real %u hr=%08lx", idx, real, hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_EnumAdapters1(IDXGIFactory1* self, UINT idx, IDXGIAdapter1** out) {
    UINT real = MapAdapterIndex(self, idx);
    HRESULT hr = o_EnumAdapters1(self, real, out);
    if (SUCCEEDED(hr) && out && *out) {
        HookAdapter(*out);
        char an[128]; DescribeAdapter(*out, an, sizeof an);
        Log("EnumAdapters1(%u) -> real %u %s", idx, real, an);
    } else Log("EnumAdapters1(%u) -> real %u hr=%08lx", idx, real, hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_EnumByPref(IDXGIFactory6* self, UINT idx, DXGI_GPU_PREFERENCE pref, REFIID riid, void** out) {
    HRESULT hr = o_EnumByPref(self, idx, pref, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        HookAdapter(*out);
        char an[128]; DescribeAdapter((IDXGIAdapter*)*out, an, sizeof an);
        Log("EnumAdapterByGpuPreference(%u, pref=%d) -> %s", idx, (int)pref, an);
    } else Log("EnumAdapterByGpuPreference(%u, pref=%d) hr=%08lx", idx, (int)pref, hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_EnumByLuid(IDXGIFactory4* self, LUID luid, REFIID riid, void** out) {
    HRESULT hr = o_EnumByLuid(self, luid, riid, out);
    if (SUCCEEDED(hr) && out && *out) HookAdapter(*out);
    Log("EnumAdapterByLuid hr=%08lx", hr);
    return hr;
}

static void HookFactory(IUnknown* f) {
    SrwGuard lk(&g_hookLock);
    void** vt = VT(f);
    bool any = false;
    any |= Patch(&vt[7], (void*)&H_EnumAdapters, (void**)&o_EnumAdapters);
    IDXGIFactory1* f1 = nullptr;
    if (SUCCEEDED(f->QueryInterface(IID_PPV_ARGS(&f1)))) { any |= Patch(&vt[12], (void*)&H_EnumAdapters1, (void**)&o_EnumAdapters1); f1->Release(); }
    IDXGIFactory4* f4 = nullptr;
    if (SUCCEEDED(f->QueryInterface(IID_PPV_ARGS(&f4)))) { any |= Patch(&vt[26], (void*)&H_EnumByLuid, (void**)&o_EnumByLuid); f4->Release(); }
    IDXGIFactory6* f6 = nullptr;
    if (SUCCEEDED(f->QueryInterface(IID_PPV_ARGS(&f6)))) { any |= Patch(&vt[29], (void*)&H_EnumByPref, (void**)&o_EnumByPref); f6->Release(); }
    if (any) Log("hooked factory vtable %p (monitor=%ls adapters=%d outputs=%d)", vt, g_monitor, g_reorderAdapters, g_reorderOutputs);
}

// ---- Win32 monitor hooks (IAT of the game executable only) ------------------
// The game positions/sizes its window with Win32 monitor APIs, not DXGI. Make the
// target monitor look like the primary one: anything resolving to the real primary
// resolves to the target, the target carries MONITORINFOF_PRIMARY, and top-level
// windows placed on the real primary are shifted onto the target.
// Our own calls below go through this DLL's IAT, i.e. straight to user32.

struct Mons { HMONITOR target, primary; };
static Mons g_mons;
static ULONGLONG g_monsAt;
static SRWLOCK g_monsLock = SRWLOCK_INIT;

static BOOL CALLBACK EnumMonProc(HMONITOR h, HDC, LPRECT, LPARAM p) {
    Mons* m = reinterpret_cast<Mons*>(p);
    MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(h, &mi)) return TRUE;
    if (mi.dwFlags & MONITORINFOF_PRIMARY) m->primary = h;
    if (!m->target && NameMatchesTarget(mi.szDevice)) m->target = h;
    return TRUE;
}

// Returns {target, primary}; target is null when the remap is inactive.
static Mons Monitors() {
    ULONGLONG now = GetTickCount64();
    {
        SrwGuard lk(&g_monsLock);
        if (now - g_monsAt < 2000) return g_mons;
    }
    Mons m{};
    EnumDisplayMonitors(nullptr, nullptr, EnumMonProc, reinterpret_cast<LPARAM>(&m));
    if (m.target == m.primary) m.target = nullptr;
    SrwGuard lk(&g_monsLock);
    if (m.target != g_mons.target) Log("monitor remap: target=%p primary=%p", m.target, m.primary);
    g_mons = m; g_monsAt = now;
    return m;
}

static bool MonRects(const Mons& m, RECT& tr, RECT& pr) {
    MONITORINFO a{}; a.cbSize = sizeof(a);
    MONITORINFO b{}; b.cbSize = sizeof(b);
    if (!m.target || !GetMonitorInfoW(m.target, &a) || !GetMonitorInfoW(m.primary, &b)) return false;
    tr = a.rcMonitor; pr = b.rcMonitor;
    return true;
}

// Shift a top-level window rect that sits on the real primary onto the target monitor.
static bool ShiftToTarget(int& x, int& y, int w, int h) {
    Mons m = Monitors(); RECT tr, pr;
    if (!MonRects(m, tr, pr)) return false;
    POINT c{x + w / 2, y + h / 2};
    if (!PtInRect(&pr, c) || PtInRect(&tr, c)) return false;
    x += tr.left - pr.left; y += tr.top - pr.top;
    return true;
}

static volatile LONG g_traceBudget = 400;  // log the first N Win32 monitor queries
#define TRACE(...) do { if (InterlockedDecrement(&g_traceBudget) > 0) Log(__VA_ARGS__); } while (0)

static HMONITOR WINAPI H_MonitorFromWindow(HWND hwnd, DWORD flags) {
    HMONITOR r = MonitorFromWindow(hwnd, flags);
    Mons m = Monitors();
    HMONITOR o = (m.target && r == m.primary) ? m.target : r;
    TRACE("MonitorFromWindow(%p) %p -> %p", hwnd, r, o);
    return o;
}
static HMONITOR WINAPI H_MonitorFromPoint(POINT pt, DWORD flags) {
    HMONITOR r = MonitorFromPoint(pt, flags);
    Mons m = Monitors();
    HMONITOR o = (m.target && r == m.primary) ? m.target : r;
    TRACE("MonitorFromPoint(%ld,%ld) %p -> %p", pt.x, pt.y, r, o);
    return o;
}
static BOOL WINAPI H_GetMonitorInfoW(HMONITOR h, LPMONITORINFO mi) {
    BOOL ok = GetMonitorInfoW(h, mi);
    Mons m = Monitors();
    if (ok && m.target) {
        if (h == m.target) mi->dwFlags |= MONITORINFOF_PRIMARY;
        else if (h == m.primary) mi->dwFlags &= ~MONITORINFOF_PRIMARY;
    }
    if (ok) TRACE("GetMonitorInfoW(%p) rc=%ld,%ld,%ld,%ld flags=%lx", h, mi->rcMonitor.left, mi->rcMonitor.top, mi->rcMonitor.right, mi->rcMonitor.bottom, mi->dwFlags);
    return ok;
}
static int WINAPI H_GetSystemMetrics(int i) {
    int r = GetSystemMetrics(i);
    if (i == SM_CXSCREEN || i == SM_CYSCREEN) {
        Mons m = Monitors(); RECT tr, pr;
        if (MonRects(m, tr, pr)) r = i == SM_CXSCREEN ? tr.right - tr.left : tr.bottom - tr.top;
    }
    if (i <= 1 || (i >= 76 && i <= 80)) TRACE("GetSystemMetrics(%d) -> %d", i, r);
    return r;
}

// Device-name based APIs: NULL means "primary display" -> use the target's GDI name.
static bool TargetDeviceName(wchar_t* out) {
    Mons m = Monitors();
    MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
    if (!m.target || !GetMonitorInfoW(m.target, &mi)) return false;
    wcscpy_s(out, CCHDEVICENAME, mi.szDevice);
    return true;
}
static BOOL WINAPI H_EnumDisplaySettingsW(LPCWSTR dev, DWORD mode, DEVMODEW* dm) {
    wchar_t t[CCHDEVICENAME];
    LPCWSTR use = (!dev && TargetDeviceName(t)) ? t : dev;
    BOOL ok = EnumDisplaySettingsW(use, mode, dm);
    if (mode == ENUM_CURRENT_SETTINGS || mode == ENUM_REGISTRY_SETTINGS)
        TRACE("EnumDisplaySettingsW(%ls -> %ls, %ld) %lux%lu @%ld,%ld", dev ? dev : L"NULL", use ? use : L"NULL", (long)mode,
              ok ? dm->dmPelsWidth : 0, ok ? dm->dmPelsHeight : 0, ok ? dm->dmPosition.x : 0, ok ? dm->dmPosition.y : 0);
    return ok;
}
static LONG WINAPI H_ChangeDisplaySettingsExW(LPCWSTR dev, DEVMODEW* dm, HWND hwnd, DWORD flags, LPVOID lp) {
    wchar_t t[CCHDEVICENAME];
    LPCWSTR use = (!dev && TargetDeviceName(t)) ? t : dev;
    LONG r = ChangeDisplaySettingsExW(use, dm, hwnd, flags, lp);
    Log("ChangeDisplaySettingsExW(%ls -> %ls, %lux%lu, flags=%lx) = %ld", dev ? dev : L"NULL", use ? use : L"NULL",
        dm ? dm->dmPelsWidth : 0, dm ? dm->dmPelsHeight : 0, flags, r);
    return r;
}
// Adapter-level enumeration: move the primary flag to the target's display device.
static BOOL WINAPI H_EnumDisplayDevicesW(LPCWSTR dev, DWORD idx, PDISPLAY_DEVICEW dd, DWORD flags) {
    BOOL ok = EnumDisplayDevicesW(dev, idx, dd, flags);
    if (ok && !dev) {
        wchar_t t[CCHDEVICENAME];
        if (TargetDeviceName(t)) {
            if (_wcsicmp(dd->DeviceName, t) == 0) dd->StateFlags |= DISPLAY_DEVICE_PRIMARY_DEVICE;
            else dd->StateFlags &= ~DISPLAY_DEVICE_PRIMARY_DEVICE;
        }
        TRACE("EnumDisplayDevicesW(NULL, %lu) %ls flags=%lx", idx, dd->DeviceName, dd->StateFlags);
    }
    return ok;
}
static BOOL WINAPI H_AdjustWindowRectEx(LPRECT r, DWORD style, BOOL menu, DWORD ex) {
    RECT in = *r;
    BOOL ok = AdjustWindowRectEx(r, style, menu, ex);
    TRACE("AdjustWindowRectEx(%ld,%ld,%ld,%ld style=%lx) -> %ld,%ld,%ld,%ld", in.left, in.top, in.right, in.bottom, style, r->left, r->top, r->right, r->bottom);
    return ok;
}
static HWND WINAPI H_CreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int w, int h,
                                     HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    if (!(style & WS_CHILD)) {
        int ox = x, oy = y;
        if (x == CW_USEDEFAULT) {
            Mons m = Monitors(); RECT tr, pr;
            if (w != CW_USEDEFAULT && MonRects(m, tr, pr)) {
                x = tr.left + ((tr.right - tr.left) - w) / 2;
                y = tr.top + ((tr.bottom - tr.top) - h) / 2;
            }
        } else {
            ShiftToTarget(x, y, w, h);
        }
        if (x != ox || y != oy) Log("CreateWindowExW '%ls' %d,%d %dx%d -> %d,%d", name ? name : L"", ox, oy, w, h, x, y);
    }
    return CreateWindowExW(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
}
static bool IsTopLevel(HWND hwnd) { return !(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD); }
static BOOL WINAPI H_SetWindowPos(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags) {
    if (!(flags & SWP_NOMOVE) && IsTopLevel(hwnd)) {
        int w = cx, h = cy;
        if (flags & SWP_NOSIZE) { RECT r; GetWindowRect(hwnd, &r); w = r.right - r.left; h = r.bottom - r.top; }
        int ox = x, oy = y;
        if (ShiftToTarget(x, y, w, h)) Log("SetWindowPos %d,%d %dx%d -> %d,%d", ox, oy, w, h, x, y);
    }
    return SetWindowPos(hwnd, after, x, y, cx, cy, flags);
}
static BOOL WINAPI H_MoveWindow(HWND hwnd, int x, int y, int w, int h, BOOL repaint) {
    if (IsTopLevel(hwnd)) {
        int ox = x, oy = y;
        if (ShiftToTarget(x, y, w, h)) Log("MoveWindow %d,%d %dx%d -> %d,%d", ox, oy, w, h, x, y);
    }
    return MoveWindow(hwnd, x, y, w, h, repaint);
}

static void PatchIAT(HMODULE mod, const char* dll, const char* fn, void* hook) {
    auto base = reinterpret_cast<BYTE*>(mod);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return;
    for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<char*>(base + imp->Name), dll) != 0) continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<char*>(ibn->Name), fn) != 0) continue;
            DWORD old;
            if (VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                iat->u1.Function = reinterpret_cast<ULONG_PTR>(hook);
                VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
                Log("IAT hook %s!%s", dll, fn);
            }
        }
    }
}

static void InstallWin32Hooks() {
    HMODULE exe = GetModuleHandleW(nullptr);
    PatchIAT(exe, "USER32.dll", "MonitorFromWindow", (void*)&H_MonitorFromWindow);
    PatchIAT(exe, "USER32.dll", "MonitorFromPoint", (void*)&H_MonitorFromPoint);
    PatchIAT(exe, "USER32.dll", "GetMonitorInfoW", (void*)&H_GetMonitorInfoW);
    PatchIAT(exe, "USER32.dll", "GetSystemMetrics", (void*)&H_GetSystemMetrics);
    PatchIAT(exe, "USER32.dll", "CreateWindowExW", (void*)&H_CreateWindowExW);
    PatchIAT(exe, "USER32.dll", "SetWindowPos", (void*)&H_SetWindowPos);
    PatchIAT(exe, "USER32.dll", "MoveWindow", (void*)&H_MoveWindow);
    PatchIAT(exe, "USER32.dll", "EnumDisplaySettingsW", (void*)&H_EnumDisplaySettingsW);
    PatchIAT(exe, "USER32.dll", "ChangeDisplaySettingsExW", (void*)&H_ChangeDisplaySettingsExW);
    PatchIAT(exe, "USER32.dll", "EnumDisplayDevicesW", (void*)&H_EnumDisplayDevicesW);
    PatchIAT(exe, "USER32.dll", "AdjustWindowRectEx", (void*)&H_AdjustWindowRectEx);
}

// ---- lazy load of the real dxgi ---------------------------------------------
// Everything here may run before DllMain / CRT init (see note at the top): Win32 only.
static SRWLOCK g_loadLock = SRWLOCK_INIT;
static volatile LONG g_loaded;
static DWORD g_loadErr;

struct Deferred { int idx; UINT64 a, b, c, d; };
static Deferred g_deferred[32];
static volatile LONG g_deferredCount;

static HMODULE SelfModule() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&SelfModule), &m);
    return m;
}

static void ReplayDeferred() {
    LONG n = g_deferredCount; if (n > 32) n = 32;
    for (LONG i = 0; i < n; ++i) {
        const Deferred& d = g_deferred[i];
        if (g_real[d.idx]) ((UINT64(WINAPI*)(UINT64, UINT64, UINT64, UINT64))g_real[d.idx])(d.a, d.b, d.c, d.d);
        Log("replayed deferred %s(%llx, %llx, %llx, %llx)", kExportNames[d.idx], d.a, d.b, d.c, d.d);
    }
    g_deferredCount = 0;
}

static volatile DWORD g_loadingThread;

extern "C" void EnsureReal() {
    if (g_loaded) return;
    // Loading the real dxgi triggers the app-compat shim, which calls our compat stubs
    // again on this same thread: don't re-enter (the stub defers that call instead).
    if (g_loadingThread == GetCurrentThreadId()) return;
    SrwGuard lk(&g_loadLock);
    if (g_loaded) return;
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH); lstrcatW(sys, L"\\dxgi.dll");
    g_loadingThread = GetCurrentThreadId();
    HMODULE m = LoadLibraryExW(sys, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    g_loadErr = GetLastError();
    g_loadingThread = 0;
    if (!m || m == SelfModule()) return;  // not possible yet (e.g. inside a loader notification): retry later
    FARPROC tmp[20];
    for (int i = 0; i < 20; ++i) tmp[i] = GetProcAddress(m, kExportNames[i]);
    if (!tmp[10]) return;
    g_sys = m;
    for (int i = 0; i < 20; ++i) g_real[i] = tmp[i];
    InterlockedExchange(&g_loaded, 1);
    Log("system dxgi loaded: %p (%ls), monitor=%ls", g_sys, sys, g_monitor);
    ReplayDeferred();
}

// Called by the asm stubs when g_real[idx] is still null. regs = { r9, r8, rdx, rcx }.
// Returns 1 if the real export is now available (stub jumps to it), 0 if the call was
// deferred (stub returns 0).
extern "C" int StubSlowPath(int idx, const UINT64* regs) {
    EnsureReal();
    if (g_real[idx]) return 1;
    LONG slot = InterlockedIncrement(&g_deferredCount) - 1;
    if (slot < 32) g_deferred[slot] = Deferred{idx, regs[3], regs[2], regs[1], regs[0]};
    return 0;
}

// ---- exported factory creators -------------------------------------------
extern "C" HRESULT WINAPI Proxy_CreateDXGIFactory(REFIID riid, void** out) {
    EnsureReal();
    if (!g_real[9]) return E_FAIL;
    HRESULT hr = ((HRESULT(WINAPI*)(REFIID, void**))g_real[9])(riid, out);
    if (SUCCEEDED(hr) && out && *out) HookFactory((IUnknown*)*out);
    Log("CreateDXGIFactory hr=%08lx", hr);
    return hr;
}
extern "C" HRESULT WINAPI Proxy_CreateDXGIFactory1(REFIID riid, void** out) {
    Log("CreateDXGIFactory1 called");
    EnsureReal();
    if (!g_real[10]) return E_FAIL;
    HRESULT hr = ((HRESULT(WINAPI*)(REFIID, void**))g_real[10])(riid, out);
    if (SUCCEEDED(hr) && out && *out) HookFactory((IUnknown*)*out);
    Log("CreateDXGIFactory1 hr=%08lx", hr);
    return hr;
}
extern "C" HRESULT WINAPI Proxy_CreateDXGIFactory2(UINT flags, REFIID riid, void** out) {
    EnsureReal();
    if (!g_real[11]) return E_FAIL;
    HRESULT hr = ((HRESULT(WINAPI*)(UINT, REFIID, void**))g_real[11])(flags, riid, out);
    if (SUCCEEDED(hr) && out && *out) HookFactory((IUnknown*)*out);
    Log("CreateDXGIFactory2 hr=%08lx", hr);
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
        GetModuleFileNameW(h, g_dir, MAX_PATH);
        if (wchar_t* s = wcsrchr(g_dir, L'\\')) s[1] = 0;
        LoadConfig();  // no LoadLibrary here: we may be loaded as a static import under the loader lock
        InterlockedExchange(&g_ready, 1);
        Log("DllMain attach, dir=%ls, real loaded=%ld (last err %lu), deferred calls=%ld", g_dir, g_loaded, g_loadErr, g_deferredCount);
        if (g_hookWin32) InstallWin32Hooks();
    }
    return TRUE;
}
