// =============================================================================
//  d3d12proxy.cpp  -  d3d12.dll proxy (feature-level override) + diagnostic logger
//
//  Based on tuffee88/d3d12ProxyEdrDx11_0. Exports the same 18 functions with the
//  same ordinals. What the original did is kept:
//    * D3D12CreateDevice is called with MinimumFeatureLevel lowered to 11_0
//      (configurable), everything else is forwarded to the real d3d12.dll
//    * the real DLL is d3d12_original.dll next to the game if present,
//      otherwise System32\d3d12.dll
//
//  Added (all written to <game dir>\d3d12proxy_logs\):
//    * a text log, flushed line by line so it survives a crash
//    * startup info: exe + version, Windows build, CPU, RAM, GPUs and driver
//      versions, which d3d12.dll / D3D12Core.dll is used (Agility SDK)
//    * export check: real d3d12.dll exports vs this proxy's, and the game's
//      d3d12 imports vs this proxy's exports
//    * every proxied D3D12 call with its arguments and the HRESULT it returned
//    * the device-factory path (D3D12GetInterface -> ID3D12DeviceFactory::
//      CreateDevice) which newer Agility-SDK games use instead of
//      D3D12CreateDevice; the same feature-level override is applied there
//    * the created device's real capabilities (max feature level, shader model,
//      binding tier, wave ops, mesh shaders, ray tracing, ...)
//    * ID3D12Device vtable hooks: every CheckFeatureSupport the game makes, and
//      every failed Create* call (PSOs, root signatures, resources, heaps ...)
//      with the description and the shaders of failing PSOs saved to disk
//    * D3D12 root-signature serialization errors (error blob text)
//    * device-removed reason + DRED breadcrumbs / page-fault data
//    * crash reports: vectored + unhandled exception handler, registers,
//      call stack, module list, breadcrumbs, minidump
//    * the game's own OutputDebugString output (incl. D3D12 debug layer)
//
//  Build: mingw-w64 + Microsoft DirectX-Headers, see build.sh
// =============================================================================

#define __USE_MINGW_ANSI_STDIO 1
#define _WIN32_WINNT 0x0A00
#define PSAPI_VERSION 2
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define __REQUIRED_RPCNDR_H_VERSION__ 475   // mingw's rpcndr.h is 475; DirectX-Headers ask for 500
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include <stdint.h>
#include <limits.h>
#include "names_gen.h"

#define PROXY_VERSION "1.5.0-logger"

// ------------------------------------------------------------------ vtable indices
// Verified against the DirectX-Headers C vtables by vtbl_check.c at build time.
#include "vtbl_idx.h"

// =============================================================================
//  Globals
// =============================================================================
struct Config {
    D3D_FEATURE_LEVEL forceFL     = D3D_FEATURE_LEVEL_11_0; // 0 = don't touch
    int  useOriginalDll           = 1;
    int  patchDeviceFactory       = 1;
    int  hookDevice               = 1;
    int  logFeatureChecks         = 1;
    int  maxFeatureLines          = 400;
    int  dumpFailedShaders        = 1;
    int  enableDRED               = 1;
    int  debugLayer               = 0;
    int  debugLayerWarnings       = 0;
    int  pixAttach                = 0;
    int  captureDebugOutput       = 1;
    int  maxDebugOutputLines      = 500;
    int  keepFilterOnTop          = 1;
    int  maxFirstChanceLines      = 60;
    int  maxFirstChanceReports    = 3;
    int  firstChanceDumps         = 0;
    int  logCppExceptions         = 0;
    int  fullDump                 = 0;
    int  heartbeatSeconds         = 30;
    int  pollDeviceRemoved        = 1;
    int  codeSnapshots            = 1;
    int  trackResources           = 1;
    int  imageDump                = 0;
    int  patchRemapFallback       = 1;   // 1 = patch code (exception-handler fix if that fails), 2 = exception-handler fix only
    int  forceRelocation          = 0;
    int  forceRelocList           = 0;
    int  tileBacking              = 0;
    int  syncSubmits              = 0;
    int  recordLists              = 0;
    int  skipDraws                = 0;
    int  skipClears               = 0;
    int  reservedAsCommitted      = 0;
    int  oomRetries               = 0;
    int  oomSysMem                = 0;
    int  skipDispatches           = 0;
    wchar_t skipShaders[256]      = L"";
    int  agilityOverride          = 0;
    wchar_t agilityPath[MAX_PATH] = L".\\D3D12_619\\";
    wchar_t originalDll[MAX_PATH] = L"d3d12_original.dll";
    int  testStep                 = 0;
};

// [Spoof]: capability answers given to the game instead of the real ones (-1 = real value)
struct Spoof {
    int maxFeatureLevel = -1, highestShaderModel = -1;
    int resourceBindingTier = -1, tiledResourcesTier = -1, resourceHeapTier = -1, conservativeRasterTier = -1;
    int rovs = -1, typedUAVLoadAdditionalFormats = -1, psStencilRef = -1, vpRtArrayIndex = -1;
    int waveOps = -1, int64ShaderOps = -1, barycentrics = -1, native16Bit = -1;
    int raytracingTier = -1, renderPassesTier = -1, vrsTier = -1, meshShaderTier = -1, samplerFeedbackTier = -1;
    int enhancedBarriers = -1, gpuUploadHeap = -1, tightAlignmentTier = -1;
    bool any = false;
};
static Spoof g_spoof;
static Config g_cfg;

static HMODULE  g_self, g_real;
static wchar_t  g_selfDir[MAX_PATH], g_exePath[MAX_PATH], g_exeDir[MAX_PATH];
static wchar_t  g_logDir[MAX_PATH], g_dumpDir[MAX_PATH], g_realPath[MAX_PATH], g_iniPath[MAX_PATH];
static const wchar_t* g_exeName = L"unknown.exe";
static HANDLE   g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_logCs;
static LARGE_INTEGER g_qpcFreq, g_qpcStart;
static DWORD    g_tls = TLS_OUT_OF_INDEXES;
static uint64_t g_skipHash[8]; static int g_skipCount;
static volatile LONG g_lateInitDone, g_preDeviceDone, g_deviceCount, g_deviceRemovedReported;
static volatile LONG g_firstChance, g_debugLines, g_featureLines, g_shaderDumps, g_failLines;
static ID3D12Device* volatile g_lastDevice;           // AddRef'd, for device-removed polling
static void**   g_hookedDeviceVtbl;
static HMODULE  g_dbghelp;
static LPTOP_LEVEL_EXCEPTION_FILTER g_gameFilter;       // filter the game tried to install
static bool     g_filterPatched;

enum : uintptr_t { TLS_IN_VEH = 1, TLS_IN_LOG = 2, TLS_INTERNAL = 4 };

// counters shown in the heartbeat
static volatile LONG c_createDevice, c_featureChecks, c_gpso, c_cpso, c_pso, c_rootsig, c_res, c_heap, c_descheap, c_fail;

// =============================================================================
//  Real exports
// =============================================================================
typedef HRESULT (WINAPI *PFN_CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
typedef HRESULT (WINAPI *PFN_GetDebugInterface)(REFIID, void**);
typedef HRESULT (WINAPI *PFN_GetInterface)(REFCLSID, REFIID, void**);
typedef HRESULT (WINAPI *PFN_EnableExperimentalFeatures)(UINT, const IID*, void*, UINT*);
typedef HRESULT (WINAPI *PFN_SerializeRS)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
typedef HRESULT (WINAPI *PFN_SerializeVRS)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
typedef HRESULT (WINAPI *PFN_CreateRSD)(LPCVOID, SIZE_T, REFIID, void**);

static PFN_CreateDevice               r_CreateDevice;
static PFN_GetDebugInterface          r_GetDebugInterface;
static PFN_GetInterface               r_GetInterface;
static PFN_EnableExperimentalFeatures r_EnableExperimentalFeatures;
static PFN_SerializeRS                r_SerializeRootSignature;
static PFN_SerializeVRS               r_SerializeVersionedRootSignature;
static PFN_CreateRSD                  r_CreateRootSignatureDeserializer;
static PFN_CreateRSD                  r_CreateVersionedRootSignatureDeserializer;

// Exports whose prototypes are unknown/internal are forwarded with a plain
// "jmp [g_fwd+8*i]" stub (registers and stack untouched). The original used
// one shared global for all of them, which races between threads.
#define FWD_COUNT 10
static const char* const kFwdNames[FWD_COUNT] = {
    "D3D12CoreCreateLayeredDevice", "D3D12CoreGetLayeredDeviceSize", "D3D12CoreRegisterLayers",
    "D3D12DeviceRemovedExtendedData", "D3D12PIXEventsReplaceBlock", "D3D12PIXGetThreadInfo",
    "D3D12PIXNotifyWakeFromFenceSignal", "D3D12PIXReportCounter", "GetBehaviorValue",
    "SetAppCompatStringPointer" };

static const char* const kProxyExports[] = {
    "D3D12CoreCreateLayeredDevice", "D3D12CoreGetLayeredDeviceSize", "D3D12CoreRegisterLayers",
    "D3D12CreateDevice", "D3D12CreateRootSignatureDeserializer", "D3D12CreateVersionedRootSignatureDeserializer",
    "D3D12DeviceRemovedExtendedData", "D3D12EnableExperimentalFeatures", "D3D12GetDebugInterface",
    "D3D12GetInterface", "D3D12PIXEventsReplaceBlock", "D3D12PIXGetThreadInfo",
    "D3D12PIXNotifyWakeFromFenceSignal", "D3D12PIXReportCounter", "D3D12SerializeRootSignature",
    "D3D12SerializeVersionedRootSignature", "GetBehaviorValue", "SetAppCompatStringPointer" };

extern "C" { void* g_fwd[FWD_COUNT]; }
extern "C" HRESULT MissingExportC(int idx);

#define FWD_STUB(i, name) \
    "\t.globl Proxy_" #name "\n\t.def Proxy_" #name "; .scl 2; .type 32; .endef\n\t.p2align 4\n" \
    "Proxy_" #name ":\n\tjmp *g_fwd+" #i "*8(%rip)\n" \
    "\t.def MissingStub_" #i "; .scl 3; .type 32; .endef\n\t.p2align 4\n" \
    "MissingStub_" #i ":\n\tmovl $" #i ", %ecx\n\tjmp MissingExportC\n"

asm(".text\n"
    FWD_STUB(0, D3D12CoreCreateLayeredDevice)
    FWD_STUB(1, D3D12CoreGetLayeredDeviceSize)
    FWD_STUB(2, D3D12CoreRegisterLayers)
    FWD_STUB(3, D3D12DeviceRemovedExtendedData)
    FWD_STUB(4, D3D12PIXEventsReplaceBlock)
    FWD_STUB(5, D3D12PIXGetThreadInfo)
    FWD_STUB(6, D3D12PIXNotifyWakeFromFenceSignal)
    FWD_STUB(7, D3D12PIXReportCounter)
    FWD_STUB(8, GetBehaviorValue)
    FWD_STUB(9, SetAppCompatStringPointer)
    "\t.section .rdata,\"dr\"\n\t.p2align 3\n\t.globl g_missing_init\n"
    "g_missing_init:\n"
    "\t.quad MissingStub_0, MissingStub_1, MissingStub_2, MissingStub_3, MissingStub_4\n"
    "\t.quad MissingStub_5, MissingStub_6, MissingStub_7, MissingStub_8, MissingStub_9\n"
    "\t.text\n");
extern "C" void* const g_missing_init[FWD_COUNT];

// =============================================================================
//  Small helpers
// =============================================================================
static uintptr_t TlsFlags() {
    if (g_tls == TLS_OUT_OF_INDEXES) return 0;
    DWORD e = GetLastError();
    uintptr_t v = (uintptr_t)TlsGetValue(g_tls);
    SetLastError(e);
    return v;
}
static void TlsSet(uintptr_t f, bool on) {
    if (g_tls == TLS_OUT_OF_INDEXES) return;
    DWORD e = GetLastError();
    uintptr_t v = (uintptr_t)TlsGetValue(g_tls);
    TlsSetValue(g_tls, (void*)(on ? (v | f) : (v & ~f)));
    SetLastError(e);
}
struct InternalScope { bool prev; InternalScope() { prev = TlsFlags() & TLS_INTERNAL; TlsSet(TLS_INTERNAL, true); } ~InternalScope() { if (!prev) TlsSet(TLS_INTERNAL, false); } };
static bool IsInternal() { return TlsFlags() & TLS_INTERNAL; }

// rotating scratch buffers for name formatting
static char* Scratch() {
    static char bufs[64][160];
    static volatile LONG idx;
    return bufs[(InterlockedIncrement(&idx)) & 63];
}

struct U8 {
    char b[1024];
    explicit U8(const wchar_t* w) { b[0] = 0; if (w) WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof b, nullptr, nullptr); b[sizeof b - 1] = 0; }
};

static const wchar_t* BaseName(const wchar_t* p) { const wchar_t* s = wcsrchr(p, L'\\'); return s ? s + 1 : p; }

static bool Readable(const void* p, size_t n) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi;
    const char* c = (const char*)p;
    while (n) {
        if (!VirtualQuery(c, &mbi, sizeof mbi)) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        size_t avail = (const char*)mbi.BaseAddress + mbi.RegionSize - c;
        if (avail >= n) break;
        c += avail; n -= avail;
    }
    return true;
}

static uint64_t Fnv1a(const void* d, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= ((const uint8_t*)d)[i]; h *= 1099511628211ull; }
    return h;
}

static const char* EnumLookup(const EnumName* t, long long v) {
    for (; t->n; t++) if (t->v == v) return t->n;
    return nullptr;
}
static const char* FeatureName(int f) { const char* n = EnumLookup(kFeatureNames, f); if (n) return n; char* s = Scratch(); snprintf(s, 160, "FEATURE_%d", f); return s; }
static const char* FormatName(int f)  { const char* n = EnumLookup(kFormatNames, f);  if (n) return n; char* s = Scratch(); snprintf(s, 160, "FORMAT_%d", f); return s; }

static const char* FlName(D3D_FEATURE_LEVEL fl) {
    switch ((int)fl) {
    case 0: return "none";
    case 0x9100: return "9_1"; case 0x9200: return "9_2"; case 0x9300: return "9_3";
    case 0xa000: return "10_0"; case 0xa100: return "10_1";
    case 0xb000: return "11_0"; case 0xb100: return "11_1";
    case 0xc000: return "12_0"; case 0xc100: return "12_1"; case 0xc200: return "12_2";
    case 0x1000: return "1_0_CORE"; case 0x1100: return "1_0_GENERIC";
    }
    char* s = Scratch(); snprintf(s, 160, "0x%x", (unsigned)fl); return s;
}

static const char* HrName(HRESULT hr) {
    const char* n = nullptr;
    switch ((uint32_t)hr) {
    case 0x00000000: n = "S_OK"; break;
    case 0x00000001: n = "S_FALSE"; break;
    case 0x80004001: n = "E_NOTIMPL"; break;
    case 0x80004002: n = "E_NOINTERFACE"; break;
    case 0x80004003: n = "E_POINTER"; break;
    case 0x80004005: n = "E_FAIL"; break;
    case 0x80070005: n = "E_ACCESSDENIED"; break;
    case 0x8007000E: n = "E_OUTOFMEMORY"; break;
    case 0x80070057: n = "E_INVALIDARG"; break;
    case 0x8007007E: n = "ERROR_MOD_NOT_FOUND"; break;
    case 0x8007007F: n = "ERROR_PROC_NOT_FOUND"; break;
    case 0x887A0001: n = "DXGI_ERROR_INVALID_CALL"; break;
    case 0x887A0002: n = "DXGI_ERROR_NOT_FOUND"; break;
    case 0x887A0003: n = "DXGI_ERROR_MORE_DATA"; break;
    case 0x887A0004: n = "DXGI_ERROR_UNSUPPORTED"; break;
    case 0x887A0005: n = "DXGI_ERROR_DEVICE_REMOVED"; break;
    case 0x887A0006: n = "DXGI_ERROR_DEVICE_HUNG"; break;
    case 0x887A0007: n = "DXGI_ERROR_DEVICE_RESET"; break;
    case 0x887A000A: n = "DXGI_ERROR_WAS_STILL_DRAWING"; break;
    case 0x887A0020: n = "DXGI_ERROR_DRIVER_INTERNAL_ERROR"; break;
    case 0x887A0022: n = "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE"; break;
    case 0x887A0026: n = "DXGI_ERROR_ACCESS_LOST"; break;
    case 0x887A0028: n = "DXGI_ERROR_SESSION_DISCONNECTED"; break;
    case 0x887A002B: n = "DXGI_ERROR_ACCESS_DENIED"; break;
    case 0x887A002D: n = "DXGI_ERROR_SDK_COMPONENT_MISSING"; break;
    case 0x887E0001: n = "D3D12_ERROR_ADAPTER_NOT_FOUND"; break;
    case 0x887E0002: n = "D3D12_ERROR_DRIVER_VERSION_MISMATCH"; break;
    case 0x887E0003: n = "D3D12_ERROR_INVALID_REDIST"; break;
    }
    char* s = Scratch();
    if (n) snprintf(s, 160, "%s (0x%08X)", n, (unsigned)hr);
    else   snprintf(s, 160, "0x%08X", (unsigned)hr);
    return s;
}

static const char* GuidName(REFGUID g) {
    for (const GuidEntry* t = kGuidNames; t->g; t++) if (IsEqualGUID(*t->g, g)) return t->n;
    static const struct { const GUID* g; const char* n; } extra[] = {
        { &IID_IUnknown, "IID_IUnknown" }, { &IID_IDXGIAdapter, "IID_IDXGIAdapter" },
        { &IID_IDXGIAdapter1, "IID_IDXGIAdapter1" }, { &IID_IDXGIDevice, "IID_IDXGIDevice" },
    };
    for (auto& e : extra) if (IsEqualGUID(*e.g, g)) return e.n;
    char* s = Scratch();
    snprintf(s, 160, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", (unsigned long)g.Data1, g.Data2, g.Data3,
             g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return s;
}

// module + offset for an address ("d3d12core.dll+0x1234")
static const char* AddrName(const void* a) {
    char* s = Scratch();
    HMODULE m = nullptr;
    if (a && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m) && m) {
        wchar_t p[MAX_PATH]; p[0] = 0;
        GetModuleFileNameW(m, p, MAX_PATH);
        snprintf(s, 160, "%s+0x%llx", U8(BaseName(p)).b, (unsigned long long)((const char*)a - (const char*)m));
    } else {
        snprintf(s, 160, "%p", a);
    }
    return s;
}

static bool ModuleVersion(HMODULE m, char* out, size_t n) {
    out[0] = 0;
    if (!m) return false;
    HRSRC r = FindResourceW(m, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16));
    if (!r) return false;
    HGLOBAL h = LoadResource(m, r);
    DWORD sz = SizeofResource(m, r);
    const uint8_t* p = (const uint8_t*)LockResource(h);
    if (!p) return false;
    for (DWORD i = 0; i + sizeof(VS_FIXEDFILEINFO) <= sz; i += 4) {
        if (*(const DWORD*)(p + i) == 0xFEEF04BD) {
            const VS_FIXEDFILEINFO* f = (const VS_FIXEDFILEINFO*)(p + i);
            snprintf(out, n, "%u.%u.%u.%u", HIWORD(f->dwFileVersionMS), LOWORD(f->dwFileVersionMS),
                     HIWORD(f->dwFileVersionLS), LOWORD(f->dwFileVersionLS));
            return true;
        }
    }
    return false;
}

static void FileVersion(const wchar_t* path, char* out, size_t n) {
    out[0] = 0;
    HMODULE m = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!m) { snprintf(out, n, "(cannot open, err %lu)", GetLastError()); return; }
    if (!ModuleVersion(m, out, n)) snprintf(out, n, "(no version resource)");
    FreeLibrary(m);
}

static bool FileExists(const wchar_t* p) { DWORD a = GetFileAttributesW(p); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }

// =============================================================================
//  Logging
// =============================================================================
static void LogWrite(const char* s, size_t n) {
    if (g_log != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(g_log, s, (DWORD)n, &w, nullptr); }
}

static void LogV(const char* fmt, va_list ap) {
    DWORD err = GetLastError();
    char buf[4096];
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    double t = g_qpcFreq.QuadPart ? double(now.QuadPart - g_qpcStart.QuadPart) / double(g_qpcFreq.QuadPart) : 0.0;
    int k = snprintf(buf, sizeof buf, "[%9.3f][T%5lu] ", t, GetCurrentThreadId());
    int m = vsnprintf(buf + k, sizeof buf - k - 2, fmt, ap);
    if (m < 0) m = 0;
    size_t len = k + ((size_t)m < sizeof buf - k - 2 ? (size_t)m : sizeof buf - k - 3);
    buf[len++] = '\r'; buf[len++] = '\n'; buf[len] = 0;
    EnterCriticalSection(&g_logCs);
    LogWrite(buf, len);
    LeaveCriticalSection(&g_logCs);
    // mirror to the debugger (DebugView), flagged so our VEH ignores it
    TlsSet(TLS_IN_LOG, true);
    OutputDebugStringA(buf);
    TlsSet(TLS_IN_LOG, false);
    SetLastError(err);
}

static void Log(const char* fmt, ...) __attribute__((format(__MINGW_PRINTF_FORMAT, 1, 2)));
static void Log(const char* fmt, ...) { va_list ap; va_start(ap, fmt); LogV(fmt, ap); va_end(ap); }

// Write a whole file (dumps of shaders / root signatures)
static bool WriteWholeFile(const wchar_t* path, const void* data, size_t n) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0; BOOL ok = WriteFile(h, data, (DWORD)n, &w, nullptr);
    CloseHandle(h);
    return ok && w == n;
}

// =============================================================================
//  Breadcrumbs: last D3D12 calls seen by hooks (cheap ring buffer)
// =============================================================================
struct Crumb { DWORD tick, tid; const char* what; HRESULT hr; };
static Crumb g_crumbs[64];
static volatile LONG g_crumbIdx;
static void AddCrumb(const char* what, HRESULT hr) {
    LONG i = InterlockedIncrement(&g_crumbIdx) - 1;
    Crumb& c = g_crumbs[i & 63];
    c.tick = GetTickCount(); c.tid = GetCurrentThreadId(); c.what = what; c.hr = hr;
}
static void LogCrumbs() {
    LONG end = g_crumbIdx, start = end > 64 ? end - 64 : 0;
    Log("  Last %ld D3D12 calls seen by the proxy (oldest first):", end - start);
    DWORD now = GetTickCount();
    for (LONG i = start; i < end; i++) {
        const Crumb& c = g_crumbs[i & 63];
        if (!c.what) continue;
        Log("    -%6lums T%5lu %-44s %s", now - c.tick, c.tid, c.what, HrName(c.hr));
    }
}

// =============================================================================
//  Config
// =============================================================================
static int IniInt(const wchar_t* key, int def) { return (int)GetPrivateProfileIntW(L"Proxy", key, def, g_iniPath); }

static void LoadConfig() {
    wchar_t fl[32];
    GetPrivateProfileStringW(L"Proxy", L"ForceFeatureLevel", L"11_0", fl, 32, g_iniPath);
    if      (!wcscmp(fl, L"11_0")) g_cfg.forceFL = D3D_FEATURE_LEVEL_11_0;
    else if (!wcscmp(fl, L"11_1")) g_cfg.forceFL = D3D_FEATURE_LEVEL_11_1;
    else if (!wcscmp(fl, L"12_0")) g_cfg.forceFL = D3D_FEATURE_LEVEL_12_0;
    else if (!wcscmp(fl, L"12_1")) g_cfg.forceFL = D3D_FEATURE_LEVEL_12_1;
    else g_cfg.forceFL = (D3D_FEATURE_LEVEL)0; // "0", "off", anything else
    g_cfg.useOriginalDll        = IniInt(L"UseOriginalDll", g_cfg.useOriginalDll);
    g_cfg.patchDeviceFactory    = IniInt(L"PatchDeviceFactory", g_cfg.patchDeviceFactory);
    g_cfg.hookDevice            = IniInt(L"HookDevice", g_cfg.hookDevice);
    g_cfg.logFeatureChecks      = IniInt(L"LogFeatureChecks", g_cfg.logFeatureChecks);
    g_cfg.maxFeatureLines       = IniInt(L"MaxFeatureLines", g_cfg.maxFeatureLines);
    g_cfg.dumpFailedShaders     = IniInt(L"DumpFailedShaders", g_cfg.dumpFailedShaders);
    g_cfg.enableDRED            = IniInt(L"EnableDRED", g_cfg.enableDRED);
    g_cfg.debugLayer            = IniInt(L"DebugLayer", g_cfg.debugLayer);
    g_cfg.debugLayerWarnings    = IniInt(L"DebugLayerWarnings", g_cfg.debugLayerWarnings);
    g_cfg.pixAttach             = IniInt(L"PixAttach", g_cfg.pixAttach);
    g_cfg.captureDebugOutput    = IniInt(L"CaptureDebugOutput", g_cfg.captureDebugOutput);
    g_cfg.maxDebugOutputLines   = IniInt(L"MaxDebugOutputLines", g_cfg.maxDebugOutputLines);
    g_cfg.keepFilterOnTop       = IniInt(L"KeepFilterOnTop", g_cfg.keepFilterOnTop);
    g_cfg.maxFirstChanceLines   = IniInt(L"MaxFirstChanceLines", g_cfg.maxFirstChanceLines);
    g_cfg.maxFirstChanceReports = IniInt(L"MaxFirstChanceReports", g_cfg.maxFirstChanceReports);
    g_cfg.firstChanceDumps      = IniInt(L"FirstChanceDumps", g_cfg.firstChanceDumps);
    g_cfg.logCppExceptions      = IniInt(L"LogCppExceptions", g_cfg.logCppExceptions);
    g_cfg.fullDump              = IniInt(L"FullDump", g_cfg.fullDump);
    g_cfg.heartbeatSeconds      = IniInt(L"HeartbeatSeconds", g_cfg.heartbeatSeconds);
    g_cfg.pollDeviceRemoved     = IniInt(L"PollDeviceRemoved", g_cfg.pollDeviceRemoved);
    g_cfg.codeSnapshots         = IniInt(L"CodeSnapshots", g_cfg.codeSnapshots);
    g_cfg.trackResources        = IniInt(L"TrackResources", g_cfg.trackResources);
    g_cfg.imageDump             = IniInt(L"ImageDump", g_cfg.imageDump);
    g_cfg.patchRemapFallback    = (int)GetPrivateProfileIntW(L"Patches", L"RemapFallback", g_cfg.patchRemapFallback, g_iniPath);
    g_cfg.forceRelocation       = (int)GetPrivateProfileIntW(L"Patches", L"ForceRelocation", g_cfg.forceRelocation, g_iniPath);
    g_cfg.forceRelocList        = (int)GetPrivateProfileIntW(L"Patches", L"ForceRelocList", g_cfg.forceRelocList, g_iniPath);
    g_cfg.tileBacking           = (int)GetPrivateProfileIntW(L"Patches", L"TileBacking", g_cfg.tileBacking, g_iniPath);
    g_cfg.syncSubmits           = (int)GetPrivateProfileIntW(L"Proxy", L"SyncSubmits", g_cfg.syncSubmits, g_iniPath);
    g_cfg.recordLists           = (int)GetPrivateProfileIntW(L"Proxy", L"RecordLists", g_cfg.recordLists, g_iniPath);
    g_cfg.skipDraws             = (int)GetPrivateProfileIntW(L"Patches", L"SkipDraws", 0, g_iniPath);
    g_cfg.skipDispatches        = (int)GetPrivateProfileIntW(L"Patches", L"SkipDispatches", 0, g_iniPath);
    g_cfg.skipClears            = (int)GetPrivateProfileIntW(L"Patches", L"SkipClears", 0, g_iniPath);
    g_cfg.reservedAsCommitted   = (int)GetPrivateProfileIntW(L"Patches", L"ReservedAsCommitted", g_cfg.reservedAsCommitted, g_iniPath);
    g_cfg.oomRetries            = (int)GetPrivateProfileIntW(L"Patches", L"OomRetries", g_cfg.oomRetries, g_iniPath);
    g_cfg.oomSysMem             = (int)GetPrivateProfileIntW(L"Patches", L"OomSysMem", g_cfg.oomSysMem, g_iniPath);
    GetPrivateProfileStringW(L"Patches", L"SkipShaders", L"", g_cfg.skipShaders, 256, g_iniPath);
    g_cfg.agilityOverride       = (int)GetPrivateProfileIntW(L"Agility", L"SDKVersion", 0, g_iniPath);
    GetPrivateProfileStringW(L"Agility", L"SDKPath", L".\\D3D12_619\\", g_cfg.agilityPath, MAX_PATH, g_iniPath);
    GetPrivateProfileStringW(L"Proxy", L"OriginalDll", L"d3d12_original.dll", g_cfg.originalDll, MAX_PATH, g_iniPath);
    // TestStep presets (override the individual settings above)
    g_cfg.testStep = IniInt(L"TestStep", 0);
    switch (g_cfg.testStep) {
    case 1: g_cfg.patchRemapFallback = 1; g_cfg.forceRelocation = 0; g_cfg.agilityOverride = 0; g_cfg.debugLayer = 0; break;
    case 2: g_cfg.patchRemapFallback = 1; g_cfg.forceRelocation = 0; g_cfg.agilityOverride = 619; g_cfg.debugLayer = 1; break;
    case 3: g_cfg.patchRemapFallback = 1; g_cfg.forceRelocation = 1; g_cfg.agilityOverride = 0; g_cfg.debugLayer = 0; break;
    case 4: g_cfg.patchRemapFallback = 1; g_cfg.forceRelocation = 1; g_cfg.agilityOverride = 619; g_cfg.debugLayer = 1; break;
    case 5: g_cfg.patchRemapFallback = 1; g_cfg.forceRelocation = 0; g_cfg.agilityOverride = 0; g_cfg.debugLayer = 0;
            wcscpy(g_cfg.originalDll, L"vkd3d-proton\\d3d12.dll"); g_cfg.useOriginalDll = 1; break;
    case 6: g_cfg.patchRemapFallback = 1; g_cfg.forceRelocList = 1; g_cfg.forceRelocation = 1; g_cfg.agilityOverride = 0; g_cfg.debugLayer = 0; break;
    case 7: g_cfg.patchRemapFallback = 1; g_cfg.forceRelocList = 1; g_cfg.forceRelocation = 1; g_cfg.tileBacking = 1; g_cfg.agilityOverride = 0; g_cfg.debugLayer = 0; break;
    case 9: g_cfg.patchRemapFallback = 1; g_cfg.forceRelocList = 1; g_cfg.forceRelocation = 1; g_cfg.tileBacking = 1; g_cfg.syncSubmits = 1; g_cfg.agilityOverride = 0; g_cfg.debugLayer = 0; break;
    case 10: g_cfg.patchRemapFallback = 1; g_cfg.forceRelocList = 1; g_cfg.forceRelocation = 1; g_cfg.tileBacking = 1; g_cfg.syncSubmits = 1; g_cfg.recordLists = 1; g_cfg.agilityOverride = 0; g_cfg.debugLayer = 0; break;
    case 11: g_cfg.patchRemapFallback = 1; g_cfg.forceRelocList = 1; g_cfg.forceRelocation = 1; g_cfg.reservedAsCommitted = 1; g_cfg.oomRetries = 20; g_cfg.oomSysMem = 1; g_cfg.recordLists = 1; g_cfg.agilityOverride = 0; g_cfg.debugLayer = 0; break;
    case 8: g_cfg.patchRemapFallback = 1; g_cfg.forceRelocList = 0; g_cfg.forceRelocation = 0; g_cfg.tileBacking = 1; g_cfg.agilityOverride = 0; g_cfg.debugLayer = 0; break;
    }

    auto sp = [](const wchar_t* k) { return (int)GetPrivateProfileIntW(L"Spoof", k, -1, g_iniPath); };
    auto spVer = [](const wchar_t* k, bool fl) -> int {   // "12_1" / "6_6" style
        wchar_t v[16]; GetPrivateProfileStringW(L"Spoof", k, L"", v, 16, g_iniPath);
        int a = 0, b = 0;
        if (swscanf(v, L"%d_%d", &a, &b) != 2 && swscanf(v, L"%d.%d", &a, &b) != 2) return -1;
        return fl ? ((a << 12) | (b << 8)) : ((a << 4) | b);
    };
    Spoof& s = g_spoof;
    s.maxFeatureLevel = spVer(L"MaxFeatureLevel", true);
    s.highestShaderModel = spVer(L"HighestShaderModel", false);
    s.resourceBindingTier = sp(L"ResourceBindingTier");     s.tiledResourcesTier = sp(L"TiledResourcesTier");
    s.resourceHeapTier = sp(L"ResourceHeapTier");           s.conservativeRasterTier = sp(L"ConservativeRasterizationTier");
    s.rovs = sp(L"ROVsSupported");                          s.typedUAVLoadAdditionalFormats = sp(L"TypedUAVLoadAdditionalFormats");
    s.psStencilRef = sp(L"PSSpecifiedStencilRefSupported"); s.vpRtArrayIndex = sp(L"VPAndRTArrayIndexFromAnyShader");
    s.waveOps = sp(L"WaveOps");                             s.int64ShaderOps = sp(L"Int64ShaderOps");
    s.barycentrics = sp(L"Barycentrics");                   s.native16Bit = sp(L"Native16BitShaderOps");
    s.raytracingTier = sp(L"RaytracingTier");               s.renderPassesTier = sp(L"RenderPassesTier");
    s.vrsTier = sp(L"VariableShadingRateTier");             s.meshShaderTier = sp(L"MeshShaderTier");
    s.samplerFeedbackTier = sp(L"SamplerFeedbackTier");     s.enhancedBarriers = sp(L"EnhancedBarriers");
    s.gpuUploadHeap = sp(L"GPUUploadHeap");               s.tightAlignmentTier = sp(L"TightAlignmentTier");
    const int* f = &s.maxFeatureLevel;
    for (int i = 0; i < 22; i++) if (f[i] != -1) s.any = true;
    { wchar_t* p = g_cfg.skipShaders; while (*p && g_skipCount < 8) { while (*p == L' ' || *p == L',') p++; if (!*p) break; wchar_t* e; unsigned long long v = wcstoull(p, &e, 16); if (e == p) break; if (v) g_skipHash[g_skipCount++] = v; p = e; } }
}

static const char* TestStepName(int s) {
    switch (s) {
    case 1: return "1 = baseline: remap fallback + full instrumentation";
    case 2: return "2 = runtime 1.619 from .\\D3D12_619 + D3D12 debug layer";
    case 3: return "3 = force the game's relocation pass (fills the remap table) + fallback";
    case 4: return "4 = step 3 + runtime 1.619 + debug layer";
    case 5: return "5 = vkd3d-proton (Vulkan) backend from .\\vkd3d-proton\\";
    case 6: return "6 = force the render worker's relocation list on (ForceRelocList) + fallback";
    case 7: return "7 = step 6 + TileBacking (map real memory into unmapped tiled resources)";
    case 9: return "9 = step 7 + SyncSubmits (wait for the GPU after every submit, log which one kills the device)";
    case 10: return "10 = step 9 + RecordLists (record draw/dispatch calls per command list; dump the list that hangs the GPU) + SkipShaders";
    case 8: return "8 = fallback + TileBacking only (no relocation forcing)";
    case 11: return "11 = step 6 + ReservedAsCommitted (tiled textures become ordinary committed textures) + RecordLists, no SyncSubmits";
    }
    return "0 = individual ini settings";
}
static void LogConfig() {
    Log("TestStep %s", TestStepName(g_cfg.testStep));
    Log("Config (%s%s):", U8(g_iniPath).b, FileExists(g_iniPath) ? "" : "  - file not found, using defaults");
    Log("  ForceFeatureLevel=%s UseOriginalDll=%d PatchDeviceFactory=%d HookDevice=%d LogFeatureChecks=%d DumpFailedShaders=%d",
        FlName(g_cfg.forceFL), g_cfg.useOriginalDll, g_cfg.patchDeviceFactory, g_cfg.hookDevice, g_cfg.logFeatureChecks, g_cfg.dumpFailedShaders);
    Log("  EnableDRED=%d DebugLayer=%d DebugLayerWarnings=%d PixAttach=%d CaptureDebugOutput=%d KeepFilterOnTop=%d",
        g_cfg.enableDRED, g_cfg.debugLayer, g_cfg.debugLayerWarnings, g_cfg.pixAttach, g_cfg.captureDebugOutput, g_cfg.keepFilterOnTop);
    Log("  MaxFirstChanceLines=%d MaxFirstChanceReports=%d FirstChanceDumps=%d LogCppExceptions=%d FullDump=%d HeartbeatSeconds=%d PollDeviceRemoved=%d",
        g_cfg.maxFirstChanceLines, g_cfg.maxFirstChanceReports, g_cfg.firstChanceDumps, g_cfg.logCppExceptions, g_cfg.fullDump,
        g_cfg.heartbeatSeconds, g_cfg.pollDeviceRemoved);
    Log("  CodeSnapshots=%d TrackResources=%d ImageDump=%d  [Patches] RemapFallback=%d ForceRelocation=%d ForceRelocList=%d  [Agility] SDKVersion=%d SDKPath=%s",
        g_cfg.codeSnapshots, g_cfg.trackResources, g_cfg.imageDump, g_cfg.patchRemapFallback, g_cfg.forceRelocation, g_cfg.forceRelocList,
        g_cfg.agilityOverride, g_cfg.agilityOverride ? U8(g_cfg.agilityPath).b : "(game's own)");
    if (!g_spoof.any) { Log("  [Spoof] none - the game sees the real capabilities"); return; }
    const Spoof& s = g_spoof;
    char b[900]; size_t k = snprintf(b, sizeof b, "  [Spoof] active:");
#define SP(name, v) if (v != -1 && k < sizeof b) k += snprintf(b + k, sizeof b - k, " %s=%d", name, v)
    if (s.maxFeatureLevel != -1) k += snprintf(b + k, sizeof b - k, " MaxFeatureLevel=%s", FlName((D3D_FEATURE_LEVEL)s.maxFeatureLevel));
    if (s.highestShaderModel != -1) k += snprintf(b + k, sizeof b - k, " HighestShaderModel=%x.%x", s.highestShaderModel >> 4, s.highestShaderModel & 15);
    SP("ResourceBindingTier", s.resourceBindingTier); SP("TiledResourcesTier", s.tiledResourcesTier); SP("ResourceHeapTier", s.resourceHeapTier);
    SP("ConservativeRasterizationTier", s.conservativeRasterTier); SP("ROVs", s.rovs); SP("TypedUAVLoadAdditionalFormats", s.typedUAVLoadAdditionalFormats);
    SP("PSStencilRef", s.psStencilRef); SP("VPAndRTArrayIndex", s.vpRtArrayIndex); SP("WaveOps", s.waveOps); SP("Int64ShaderOps", s.int64ShaderOps);
    SP("Barycentrics", s.barycentrics); SP("Native16Bit", s.native16Bit); SP("RaytracingTier", s.raytracingTier); SP("RenderPassesTier", s.renderPassesTier);
    SP("VariableShadingRateTier", s.vrsTier); SP("MeshShaderTier", s.meshShaderTier); SP("SamplerFeedbackTier", s.samplerFeedbackTier);
    SP("EnhancedBarriers", s.enhancedBarriers); SP("GPUUploadHeap", s.gpuUploadHeap); SP("TightAlignmentTier", s.tightAlignmentTier);
#undef SP
    Log("%s", b);
}

// =============================================================================
//  System info
// =============================================================================
static void LogSystemInfo() {
    typedef LONG (WINAPI *PFN_RtlGetVersion)(OSVERSIONINFOW*);
    OSVERSIONINFOW vi = { sizeof vi };
    PFN_RtlGetVersion rgv = (PFN_RtlGetVersion)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (rgv) rgv(&vi);
    DWORD ubr = 0, sz = sizeof ubr;
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &sz);
    bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    Log("Windows %lu.%lu build %lu.%lu%s", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber, ubr, wine ? "  (running under Wine/Proton)" : "");

    int r[4]; char brand[49] = {0};
    __asm__ __volatile__("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(0x80000000), "c"(0));
    if ((unsigned)r[0] >= 0x80000004) {
        for (int i = 0; i < 3; i++) {
            __asm__ __volatile__("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(0x80000002 + i), "c"(0));
            memcpy(brand + 16 * i, r, 16);
        }
    }
    SYSTEM_INFO si; GetNativeSystemInfo(&si);
    MEMORYSTATUSEX ms = { sizeof ms }; GlobalMemoryStatusEx(&ms);
    Log("CPU: %s (%lu logical)  RAM: %.1f GB total, %.1f GB free", brand, si.dwNumberOfProcessors,
        ms.ullTotalPhys / 1073741824.0, ms.ullAvailPhys / 1073741824.0);
}

// =============================================================================
//  PE helpers: exports of the real DLL, imports of the game exe
// =============================================================================
static IMAGE_NT_HEADERS64* NtHeaders(HMODULE m) {
    if (!m) return nullptr;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)m;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)((char*)m + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt : nullptr;
}

static bool ProxyExports(const char* name) {
    for (const char* e : kProxyExports) if (!strcmp(e, name)) return true;
    return false;
}

static void CheckExports() {
    IMAGE_NT_HEADERS64* nt = NtHeaders(g_real);
    if (!nt) return;
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dd.VirtualAddress) return;
    IMAGE_EXPORT_DIRECTORY* ed = (IMAGE_EXPORT_DIRECTORY*)((char*)g_real + dd.VirtualAddress);
    DWORD* names = (DWORD*)((char*)g_real + ed->AddressOfNames);
    int missing = 0;
    for (DWORD i = 0; i < ed->NumberOfNames; i++) {
        const char* n = (const char*)g_real + names[i];
        if (!ProxyExports(n)) { Log("  !! real d3d12.dll exports '%s' but this proxy does NOT - a game calling it will fail", n); missing++; }
    }
    Log("Export check: real d3d12.dll has %lu named exports, %d missing from the proxy", ed->NumberOfNames, missing);
}

static void CheckImportList(HMODULE exe, DWORD nameRva, DWORD thunkRva, const char* kind) {
    IMAGE_NT_HEADERS64* nt = NtHeaders(exe);
    DWORD size = nt->OptionalHeader.SizeOfImage;
    if (!thunkRva || thunkRva >= size) return;
    const char* dll = (const char*)exe + nameRva;
    IMAGE_THUNK_DATA64* t = (IMAGE_THUNK_DATA64*)((char*)exe + thunkRva);
    char list[1024]; size_t len = 0; list[0] = 0;
    for (int i = 0; i < 64 && Readable(t, sizeof *t) && t->u1.AddressOfData; i++, t++) {
        char one[160];
        if (IMAGE_SNAP_BY_ORDINAL64(t->u1.Ordinal)) {
            snprintf(one, sizeof one, "#%u", (unsigned)IMAGE_ORDINAL64(t->u1.Ordinal));
        } else {
            if (t->u1.AddressOfData >= size) break;
            const char* n = (const char*)((IMAGE_IMPORT_BY_NAME*)((char*)exe + t->u1.AddressOfData))->Name;
            snprintf(one, sizeof one, "%s%s", n, ProxyExports(n) ? "" : " [MISSING IN PROXY!]");
        }
        len += snprintf(list + len, sizeof list - len, "%s%s", len ? ", " : "", one);
        if (len >= sizeof list - 1) break;
    }
    Log("Game %s of %s: %s", kind, dll, list[0] ? list : "(none readable)");
}

static void CheckGameImports() {
    HMODULE exe = GetModuleHandleW(nullptr);
    IMAGE_NT_HEADERS64* nt = NtHeaders(exe);
    if (!nt) return;
    DWORD size = nt->OptionalHeader.SizeOfImage;
    bool found = false;
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dd.VirtualAddress && dd.VirtualAddress < size) {
        for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)((char*)exe + dd.VirtualAddress);
             Readable(d, sizeof *d) && d->Name && d->Name < size; d++) {
            if (_stricmp((const char*)exe + d->Name, "d3d12.dll")) continue;
            CheckImportList(exe, d->Name, d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk, "static imports");
            found = true;
        }
    }
    dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (dd.VirtualAddress && dd.VirtualAddress < size) {
        for (IMAGE_DELAYLOAD_DESCRIPTOR* d = (IMAGE_DELAYLOAD_DESCRIPTOR*)((char*)exe + dd.VirtualAddress);
             Readable(d, sizeof *d) && d->DllNameRVA && d->DllNameRVA < size; d++) {
            if (_stricmp((const char*)exe + d->DllNameRVA, "d3d12.dll")) continue;
            CheckImportList(exe, d->DllNameRVA, d->ImportNameTableRVA, "delay-load imports");
            found = true;
        }
    }
    if (!found) Log("Game exe has no readable d3d12.dll import entry (loaded dynamically, or the exe is packed)");
}

// Agility SDK: the exe exports D3D12SDKVersion / D3D12SDKPath when it ships its own D3D12Core.dll
static void LogAgility() {
    HMODULE exe = GetModuleHandleW(nullptr);
    UINT* ver = (UINT*)(void*)GetProcAddress(exe, "D3D12SDKVersion");
    const char** path = (const char**)(void*)GetProcAddress(exe, "D3D12SDKPath");
    if (!ver) { Log("Agility SDK: exe does not export D3D12SDKVersion -> uses the OS D3D12 runtime (System32\\D3D12Core.dll)"); }
    else {
        const char* p = (path && Readable(path, 8) && Readable(*path, 1)) ? *path : "(none)";
        Log("Agility SDK: exe exports D3D12SDKVersion=%u D3D12SDKPath=\"%s\"", *ver, p);
        if (path && *path) {
            wchar_t wp[MAX_PATH], full[MAX_PATH];
            MultiByteToWideChar(CP_UTF8, 0, *path, -1, wp, MAX_PATH);
            if (wp[0] == L'.' && (wp[1] == L'\\' || wp[1] == L'/')) swprintf(full, MAX_PATH, L"%ls\\%ls", g_exeDir, wp + 2);
            else swprintf(full, MAX_PATH, L"%ls\\%ls", g_exeDir, wp);
            size_t l = wcslen(full);
            if (l && full[l - 1] != L'\\' && full[l - 1] != L'/') wcscat(full, L"\\");
            wcscat(full, L"D3D12Core.dll");
            char v[64]; FileVersion(full, v, sizeof v);
            Log("  game's D3D12Core.dll: %s  %s", U8(full).b, FileExists(full) ? v : "-> NOT FOUND");
        }
    }
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH); wcscat(sys, L"\\D3D12Core.dll");
    char v[64]; FileVersion(sys, v, sizeof v);
    Log("  System32 D3D12Core.dll: %s", FileExists(sys) ? v : "NOT FOUND");
}

static void LogGameDirDlls() {
    wchar_t pat[MAX_PATH]; swprintf(pat, MAX_PATH, L"%ls\\*.dll", g_exeDir);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    char list[2048]; size_t len = 0; list[0] = 0;
    static const wchar_t* interesting[] = { L"d3d12", L"d3d11", L"dxgi", L"dinput8", L"version", L"winmm", L"d3d12_original",
                                            L"d3d12core", L"reshade", L"dxvk", L"vkd3d", L"nvapi", L"amd_ags", L"ffx", L"sl.", L"nvngx", L"libxess" };
    do {
        wchar_t low[MAX_PATH]; wcscpy(low, fd.cFileName); _wcslwr(low);
        for (const wchar_t* k : interesting)
            if (wcsstr(low, k)) { len += snprintf(list + len, sizeof list - len, "%s%s", len ? ", " : "", U8(fd.cFileName).b); break; }
        if (len >= sizeof list - 1) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    Log("Graphics/mod DLLs next to the exe: %s", list[0] ? list : "(none)");
}

// =============================================================================
//  Real DLL loading
// =============================================================================
static bool LoadRealDll() {
    if (g_cfg.useOriginalDll) {
        swprintf(g_realPath, MAX_PATH, L"%ls\\%ls", g_selfDir, g_cfg.originalDll);
        if (FileExists(g_realPath)) {
            g_real = LoadLibraryExW(g_realPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (g_real) Log("Real D3D12: loaded %s from the game folder", U8(g_cfg.originalDll).b);
            else Log("Real D3D12: %s exists but failed to load (err %lu), falling back to System32", U8(g_cfg.originalDll).b, GetLastError());
        } else if (g_cfg.testStep == 5) Log("!! TestStep 5: %s not found - put vkd3d-proton's x64 d3d12.dll and d3d12core.dll there. Using System32 instead.", U8(g_realPath).b);
    }
    if (!g_real) {
        GetSystemDirectoryW(g_realPath, MAX_PATH);
        wcscat(g_realPath, L"\\d3d12.dll");
        g_real = LoadLibraryW(g_realPath);
        if (!g_real) { Log("!! FATAL: could not load %s (err %lu)", U8(g_realPath).b, GetLastError()); return false; }
    }
    wchar_t p[MAX_PATH]; GetModuleFileNameW(g_real, p, MAX_PATH);
    char v[64]; ModuleVersion(g_real, v, sizeof v);
    Log("Real D3D12: %s  version %s  base %p", U8(p).b, v, (void*)g_real);
    if (_wcsicmp(p, g_realPath) == 0 && !wcsstr(p, L"\\System32\\") && !wcsstr(p, L"\\system32\\")) {
        wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH); wcscat(sys, L"\\d3d12.dll");
        char sv[64]; FileVersion(sys, sv, sizeof sv);
        Log("  System32 d3d12.dll version is %s%s", sv, strcmp(sv, v) ? "  <-- DIFFERENT from d3d12_original.dll. A stale copy can break after Windows updates; try deleting d3d12_original.dll" : "");
    }

    r_CreateDevice                         = (PFN_CreateDevice)(void*)GetProcAddress(g_real, "D3D12CreateDevice");
    r_GetDebugInterface                    = (PFN_GetDebugInterface)(void*)GetProcAddress(g_real, "D3D12GetDebugInterface");
    r_GetInterface                         = (PFN_GetInterface)(void*)GetProcAddress(g_real, "D3D12GetInterface");
    r_EnableExperimentalFeatures           = (PFN_EnableExperimentalFeatures)(void*)GetProcAddress(g_real, "D3D12EnableExperimentalFeatures");
    r_SerializeRootSignature               = (PFN_SerializeRS)(void*)GetProcAddress(g_real, "D3D12SerializeRootSignature");
    r_SerializeVersionedRootSignature      = (PFN_SerializeVRS)(void*)GetProcAddress(g_real, "D3D12SerializeVersionedRootSignature");
    r_CreateRootSignatureDeserializer      = (PFN_CreateRSD)(void*)GetProcAddress(g_real, "D3D12CreateRootSignatureDeserializer");
    r_CreateVersionedRootSignatureDeserializer = (PFN_CreateRSD)(void*)GetProcAddress(g_real, "D3D12CreateVersionedRootSignatureDeserializer");
    int missing = 0;
    for (int i = 0; i < FWD_COUNT; i++) {
        g_fwd[i] = (void*)GetProcAddress(g_real, kFwdNames[i]);
        if (!g_fwd[i]) { g_fwd[i] = g_missing_init[i]; Log("  real d3d12.dll has no export %s (calls to it will return E_NOTIMPL and be logged)", kFwdNames[i]); missing++; }
    }
    if (!r_CreateDevice) Log("!! real d3d12.dll has no D3D12CreateDevice export");
    return true;
}

// ---- Agility SDK override: the OS d3d12.dll asks the exe for D3D12SDKVersion / D3D12SDKPath.
// We hook GetProcAddress in the real d3d12.dll's import table and answer with our values.
typedef FARPROC (WINAPI *PFN_GetProcAddress)(HMODULE, LPCSTR);
static PFN_GetProcAddress r_GetProcAddress;
static UINT g_agilityVer;
static char g_agilityPathA[MAX_PATH];
static const char* g_agilityPathPtr = g_agilityPathA;
static volatile LONG g_agilityHits;
static FARPROC WINAPI My_GetProcAddress(HMODULE m, LPCSTR name) {
    if (((uintptr_t)name >> 16) && m == GetModuleHandleW(nullptr)) {
        if (!strcmp(name, "D3D12SDKVersion")) { if (InterlockedIncrement(&g_agilityHits) <= 4) Log("[agility] runtime asked the exe for D3D12SDKVersion -> answering %u (override)", g_agilityVer); return (FARPROC)(void*)&g_agilityVer; }
        if (!strcmp(name, "D3D12SDKPath"))    { if (InterlockedIncrement(&g_agilityHits) <= 4) Log("[agility] runtime asked the exe for D3D12SDKPath -> answering \"%s\" (override)", g_agilityPathA); return (FARPROC)(void*)&g_agilityPathPtr; }
    }
    return r_GetProcAddress(m, name);
}
static int HookIatByName(HMODULE mod, const char* fn, void* hook, void** orig) {
    IMAGE_NT_HEADERS64* nt = NtHeaders(mod);
    if (!nt) return 0;
    int n = 0;
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dd.VirtualAddress) {
        for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)((char*)mod + dd.VirtualAddress); d->Name; d++) {
            if (!d->OriginalFirstThunk) continue;
            IMAGE_THUNK_DATA64* names = (IMAGE_THUNK_DATA64*)((char*)mod + d->OriginalFirstThunk);
            IMAGE_THUNK_DATA64* iat = (IMAGE_THUNK_DATA64*)((char*)mod + d->FirstThunk);
            for (; names->u1.AddressOfData; names++, iat++) {
                if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
                const char* nm = (const char*)((IMAGE_IMPORT_BY_NAME*)((char*)mod + names->u1.AddressOfData))->Name;
                if (strcmp(nm, fn)) continue;
                DWORD old;
                if (VirtualProtect(&iat->u1.Function, 8, PAGE_READWRITE, &old)) {
                    if (!*orig) *orig = (void*)iat->u1.Function;
                    iat->u1.Function = (ULONGLONG)hook;
                    VirtualProtect(&iat->u1.Function, 8, old, &old);
                    n++;
                }
            }
        }
    }
    dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (dd.VirtualAddress) {
        for (IMAGE_DELAYLOAD_DESCRIPTOR* d = (IMAGE_DELAYLOAD_DESCRIPTOR*)((char*)mod + dd.VirtualAddress); d->DllNameRVA; d++) {
            IMAGE_THUNK_DATA64* names = (IMAGE_THUNK_DATA64*)((char*)mod + d->ImportNameTableRVA);
            IMAGE_THUNK_DATA64* iat = (IMAGE_THUNK_DATA64*)((char*)mod + d->ImportAddressTableRVA);
            for (; names->u1.AddressOfData; names++, iat++) {
                if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
                const char* nm = (const char*)((IMAGE_IMPORT_BY_NAME*)((char*)mod + names->u1.AddressOfData))->Name;
                if (strcmp(nm, fn)) continue;
                DWORD old;
                if (VirtualProtect(&iat->u1.Function, 8, PAGE_READWRITE, &old)) { iat->u1.Function = (ULONGLONG)hook; VirtualProtect(&iat->u1.Function, 8, old, &old); n++; }
            }
        }
    }
    return n;
}
static void SetupAgilityOverride() {
    if (!g_cfg.agilityOverride || !g_real) return;
    g_agilityVer = (UINT)g_cfg.agilityOverride;
    WideCharToMultiByte(CP_UTF8, 0, g_cfg.agilityPath, -1, g_agilityPathA, MAX_PATH, nullptr, nullptr);
    wchar_t full[MAX_PATH], core[MAX_PATH];
    const wchar_t* rel = g_cfg.agilityPath;
    if (rel[0] == L'.' && (rel[1] == L'\\' || rel[1] == L'/')) rel += 2;
    swprintf(full, MAX_PATH, L"%ls\\%ls", g_exeDir, rel);
    swprintf(core, MAX_PATH, L"%lsD3D12Core.dll", full);
    char v[64] = ""; if (FileExists(core)) FileVersion(core, v, sizeof v);
    wchar_t layers[MAX_PATH]; swprintf(layers, MAX_PATH, L"%lsd3d12SDKLayers.dll", full);
    r_GetProcAddress = (PFN_GetProcAddress)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcAddress");
    void* dummy = nullptr;
    int n = HookIatByName(g_real, "GetProcAddress", (void*)&My_GetProcAddress, &dummy);
    Log("[agility] override to SDK %u at \"%s\": D3D12Core.dll %s, d3d12SDKLayers.dll %s, GetProcAddress hooked in real d3d12.dll: %d slot(s)",
        g_agilityVer, g_agilityPathA, FileExists(core) ? v : "MISSING", FileExists(layers) ? "present" : "missing", n);
    if (!n) Log("[agility] !! could not hook - override will not take effect");
}

extern "C" HRESULT MissingExportC(int idx) {
    Log("!! game called %s, which the real d3d12.dll does not export -> returning E_NOTIMPL", (idx >= 0 && idx < FWD_COUNT) ? kFwdNames[idx] : "?");
    return E_NOTIMPL;
}

// =============================================================================
//  Optional PIX attach (original feature; off by default now - it ran in
//  DllMain unconditionally and loads a large DLL into the game)
// =============================================================================
static void PixAttach() {
    if (GetModuleHandleW(L"WinPixGpuCapturer.dll")) { Log("PIX: WinPixGpuCapturer.dll already loaded"); return; }
    wchar_t pf[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH)) return;
    wchar_t pat[MAX_PATH]; swprintf(pat, MAX_PATH, L"%ls\\Microsoft PIX\\*", pf);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    wchar_t best[MAX_PATH] = L"";
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.' && wcscmp(best, fd.cFileName) <= 0)
                wcscpy(best, fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!best[0]) { Log("PIX: not installed"); return; }
    wchar_t dll[MAX_PATH]; swprintf(dll, MAX_PATH, L"%ls\\Microsoft PIX\\%ls\\WinPixGpuCapturer.dll", pf, best);
    HMODULE m = LoadLibraryW(dll);
    Log("PIX: LoadLibrary(%s) -> %p", U8(dll).b, (void*)m);
}

// ---- resource registry: pointer -> how/where it was created (for crash reports)
struct ResRec { void* volatile key; const char* fn; DWORD tid; float t; UINT64 width; UINT height; UINT16 depth, mips; int fmt, dim, heapType, layout; UINT flags, heapFlags; };
#define RES_SLOTS (1 << 17)
static ResRec* g_res;            // allocated on first use
static volatile LONG g_resCount;
static void* volatile g_resVtbl; // vtable of D3D12Core's resource object (learned from the first resource)
static float NowSec() { LARGE_INTEGER n; QueryPerformanceCounter(&n); return float(double(n.QuadPart - g_qpcStart.QuadPart) / double(g_qpcFreq.QuadPart)); }
static ResRec* ResSlot(void* p, bool insert) {
    if (!g_res || !p) return nullptr;
    uint32_t i = (uint32_t)((((uintptr_t)p >> 4) * 0x9E3779B97F4A7C15ull) >> (64 - 17));
    for (int n = 0; n < 64; n++, i = (i + 1) & (RES_SLOTS - 1)) {
        void* k = g_res[i].key;
        if (k == p) return &g_res[i];
        if (!k) {
            if (!insert) return nullptr;
            if (InterlockedCompareExchangePointer(&g_res[i].key, p, nullptr) == nullptr) { InterlockedIncrement(&g_resCount); return &g_res[i]; }
            if (g_res[i].key == p) return &g_res[i];
        }
    }
    return nullptr;
}
static const char* HeapTypeName(int t);
static const char* DimName(int d);

// =============================================================================
//  Exceptions, crash reports, minidumps
// =============================================================================
static const char* ExcName(DWORD c) {
    switch (c) {
    case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
    case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW: return "INT_OVERFLOW";
    case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INVALID_OPERATION: return "FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW: return "FLT_OVERFLOW";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
    case EXCEPTION_INVALID_DISPOSITION: return "INVALID_DISPOSITION";
    case 0xC0000374: return "HEAP_CORRUPTION";
    case 0xC0000409: return "STACK_BUFFER_OVERRUN / fast-fail";
    case 0xC0000417: return "INVALID_CRUNTIME_PARAMETER";
    case 0xC0000420: return "ASSERTION_FAILURE";
    case 0xC06D007E: return "DELAYLOAD: module not found";
    case 0xC06D007F: return "DELAYLOAD: procedure not found";
    case 0xE06D7363: return "C++ exception (throw)";
    case 0xE0434352: return ".NET exception";
    case 0x40000015: return "FATAL_APP_EXIT";
    case 0x80000003: return "BREAKPOINT";
    case 0x80000004: return "SINGLE_STEP";
    }
    return "unknown";
}

static bool IsInteresting(DWORD c) {
    switch (c) {
    case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_IN_PAGE_ERROR: case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION: case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_STACK_OVERFLOW: case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: case EXCEPTION_NONCONTINUABLE_EXCEPTION:
    case EXCEPTION_INVALID_DISPOSITION: case 0xC0000374: case 0xC0000409: case 0xC0000417: case 0xC0000420:
    case 0xC06D007E: case 0xC06D007F: case 0x40000015:
        return true;
    case 0xE06D7363: return g_cfg.logCppExceptions != 0;
    }
    return false;
}

static void LogModules() {
    HMODULE mods[512]; DWORD need = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return;
    int n = (int)(need / sizeof(HMODULE)); if (n > 512) n = 512;
    Log("  Loaded modules (%d):", n);
    for (int i = 0; i < n; i++) {
        wchar_t p[MAX_PATH]; p[0] = 0; GetModuleFileNameW(mods[i], p, MAX_PATH);
        MODULEINFO mi = {}; K32GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi);
        char v[64]; ModuleVersion(mods[i], v, sizeof v);
        Log("    %016llx %08lx %-40s %s", (unsigned long long)mods[i], mi.SizeOfImage, U8(BaseName(p)).b, v);
    }
}

static DWORD64 g_frames[48]; static int g_frameCount;
static void WalkStack(const CONTEXT* start, int maxFrames) {
    CONTEXT c = *start;
    g_frameCount = 0;
    Log("  Call stack (unwound):");
    for (int i = 0; i < maxFrames && c.Rip; i++) {
        if (g_frameCount < 48) g_frames[g_frameCount++] = c.Rip;
        Log("    #%02d %s  (rsp=%016llx)", i, AddrName((void*)c.Rip), (unsigned long long)c.Rsp);
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
        if (fe) {
            void* hd = nullptr; DWORD64 est = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fe, &c, &hd, &est, nullptr);
        } else {
            if (!Readable((void*)c.Rsp, 8)) break;
            c.Rip = *(DWORD64*)c.Rsp; c.Rsp += 8;
        }
        if (!c.Rsp || !Readable((void*)c.Rsp, 8)) { if (c.Rip) Log("    #%02d %s", i + 1, AddrName((void*)c.Rip)); break; }
    }
}

static bool IsCodeAddr(DWORD64 a) {
    MEMORY_BASIC_INFORMATION mbi;
    if (a < 0x10000 || !VirtualQuery((void*)a, &mbi, sizeof mbi)) return false;
    return mbi.Type == MEM_IMAGE && mbi.State == MEM_COMMIT &&
           (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

static void ScanStack(DWORD64 rsp) {
    Log("  Stack scan (values that point into code; likely callers):");
    int hits = 0;
    for (int i = 0; i < 512 && hits < 24; i++) {
        DWORD64* p = (DWORD64*)rsp + i;
        if (!Readable(p, 8)) break;
        if (IsCodeAddr(*p)) { Log("    [rsp+%03x] %s", i * 8, AddrName((void*)*p)); hits++; }
    }
}

static void WriteMinidump(const char* kind, EXCEPTION_POINTERS* ep) {
    if (!g_dbghelp) {
        wchar_t p[MAX_PATH]; GetSystemDirectoryW(p, MAX_PATH); wcscat(p, L"\\dbghelp.dll");
        g_dbghelp = LoadLibraryW(p);
    }
    typedef BOOL (WINAPI *PFN_MDWD)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    PFN_MDWD mdwd = g_dbghelp ? (PFN_MDWD)(void*)GetProcAddress(g_dbghelp, "MiniDumpWriteDump") : nullptr;
    if (!mdwd) { Log("  (minidump: dbghelp.dll not available)"); return; }
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t path[MAX_PATH];
    wchar_t wkind[32]; MultiByteToWideChar(CP_UTF8, 0, kind, -1, wkind, 32);
    swprintf(path, MAX_PATH, L"%ls\\%ls_%04d%02d%02d_%02d%02d%02d_%lu.dmp", g_dumpDir, wkind, st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, GetCurrentThreadId());
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { Log("  (minidump: cannot create file, err %lu)", GetLastError()); return; }
    MINIDUMP_EXCEPTION_INFORMATION mei = { GetCurrentThreadId(), ep, FALSE };
    MINIDUMP_TYPE type = (MINIDUMP_TYPE)(g_cfg.fullDump ? (MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules)
                                                        : (MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory));
    BOOL ok = mdwd(GetCurrentProcess(), GetCurrentProcessId(), h, type, ep ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(h);
    Log("  Minidump: %s %s", U8(path).b, ok ? "written" : "FAILED");
}

// MSVC RTTI class name of a C++ object with a vtable (works for D3D12Core and most game classes)
static const char* RttiName(const void* obj) {
    if (!Readable(obj, 8)) return nullptr;
    const void* const* vt = *(const void* const* const*)obj;
    if (!Readable(vt - 1, 8)) return nullptr;
    const uint8_t* col = (const uint8_t*)vt[-1];
    if (!Readable(col, 24) || *(const DWORD*)col != 1) return nullptr;
    uintptr_t base = (uintptr_t)col - *(const DWORD*)(col + 20);       // pSelf RVA
    const char* name = (const char*)(base + *(const DWORD*)(col + 12) + 16);   // TypeDescriptor->name
    if (!Readable(name, 4) || name[0] != '.' || name[1] != '?') return nullptr;
    return name;
}

static void DescribeValue(const char* where, DWORD64 v) {
    if (v < 0x10000 || (v >> 47)) return;
    ResRec* r = ResSlot((void*)v, false);
    const void* vt = Readable((void*)v, 8) ? *(void* const*)v : nullptr;
    bool isRes = r || (g_resVtbl && vt == g_resVtbl);
    if (isRes) {
        char line[600]; size_t k = 0;
        k += snprintf(line + k, sizeof line - k, "    %-10s %016llx = ID3D12Resource", where, (unsigned long long)v);
        if (r) k += snprintf(line + k, sizeof line - k, " created by %s (T%lu, t=%.3fs): %s %llux%ux%u mips=%u %s layout=%d flags=0x%x heap=%s heapFlags=0x%x",
                             r->fn, r->tid, r->t, DimName(r->dim), (unsigned long long)r->width, r->height, r->depth, r->mips, FormatName(r->fmt), r->layout, r->flags,
                             r->heapType >= 0 ? HeapTypeName(r->heapType) : "(placed/reserved)", r->heapFlags);
        else {
            k += snprintf(line + k, sizeof line - k, " NOT created through ID3D12Device (swapchain buffer, or made by another DLL):");
            ID3D12Resource* res = (ID3D12Resource*)v;
            D3D12_RESOURCE_DESC d;
#if defined(_MSC_VER) || !defined(_WIN32)
            d = res->GetDesc();
#else
            res->GetDesc(&d);
#endif
            k += snprintf(line + k, sizeof line - k, " %s %llux%ux%u mips=%u %s flags=0x%x", DimName(d.Dimension), (unsigned long long)d.Width, d.Height, d.DepthOrArraySize, d.MipLevels, FormatName(d.Format), d.Flags);
        }
        wchar_t nm[128]; UINT nsz = sizeof nm - 2;
        if (k < sizeof line && SUCCEEDED(((ID3D12Resource*)v)->GetPrivateData(WKPDID_D3DDebugObjectNameW, &nsz, nm))) { nm[nsz / 2] = 0; snprintf(line + k, sizeof line - k, " name=\"%s\"", U8(nm).b); }
        Log("%s", line);
        return;
    }
    if (vt) {
        const char* rt = RttiName((void*)v);
        HMODULE m = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)vt, &m) && m)
            Log("    %-10s %016llx = object, vtable %s%s%s", where, (unsigned long long)v, AddrName(vt), rt ? "  class " : "", rt ? rt : "");
    }
}

static void DescribeObjects(const CONTEXT* c) {
    Log("  Objects in registers / on the stack (D3D12 resources are identified by the proxy's registry, %ld tracked):", g_resCount);
    const struct { const char* n; DWORD64 v; } regs[] = { {"RAX", c->Rax}, {"RBX", c->Rbx}, {"RCX", c->Rcx}, {"RDX", c->Rdx}, {"RSI", c->Rsi}, {"RDI", c->Rdi},
        {"RBP", c->Rbp}, {"R8", c->R8}, {"R9", c->R9}, {"R10", c->R10}, {"R11", c->R11}, {"R12", c->R12}, {"R13", c->R13}, {"R14", c->R14}, {"R15", c->R15} };
    for (auto& r : regs) DescribeValue(r.n, r.v);
    DWORD64 seen[64]; int ns = 0;
    for (int i = 0; i < 160; i++) {
        DWORD64* p = (DWORD64*)c->Rsp + i;
        if (!Readable(p, 8)) break;
        DWORD64 v = *p; bool dup = false;
        for (int j = 0; j < ns; j++) if (seen[j] == v) dup = true;
        if (dup || IsCodeAddr(v)) continue;
        if (ResSlot((void*)v, false) || (Readable((void*)v, 8) && g_resVtbl && *(void**)v == g_resVtbl)) {
            char w[24]; snprintf(w, sizeof w, "[rsp+%03x]", i * 8);
            DescribeValue(w, v);
            if (ns < 64) seen[ns++] = v;
        }
    }
}

// Save the (decrypted, in-memory) code around the crash and its callers, so it can be disassembled
static void SnapshotCode(DWORD64 addr, int idx) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)addr, &m) || !m) return;
    wchar_t mp[MAX_PATH]; GetModuleFileNameW(m, mp, MAX_PATH);
    const wchar_t* mn = BaseName(mp);
    if (!_wcsnicmp(mn, L"ntdll", 5) || !_wcsnicmp(mn, L"kernel", 6) || m == g_self) return;
    DWORD64 lo = addr - 0x800, hi = addr + 0x200;
    while (lo < addr && !Readable((void*)lo, (size_t)(addr - lo))) lo += 0x100;
    if (!Readable((void*)lo, (size_t)(hi - lo))) hi = addr + 0x10;
    if (!Readable((void*)lo, (size_t)(hi - lo))) return;
    // file: <rva of first byte>, so offsets map back to module+rva
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\code_%02d_%ls_rva%llx_frame%llx.bin", g_dumpDir, idx, mn,
             (unsigned long long)(lo - (DWORD64)m), (unsigned long long)(addr - (DWORD64)m));
    WriteWholeFile(path, (void*)lo, (size_t)(hi - lo));
}

// Write the game exe's whole in-memory image (decrypted) to a file, unreadable pages as zeros.
static void DumpExeImage() {
    HMODULE exe = GetModuleHandleW(nullptr);
    IMAGE_NT_HEADERS64* nt = NtHeaders(exe);
    if (!nt) return;
    SIZE_T size = nt->OptionalHeader.SizeOfImage;
    wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, L"%ls\\%ls.mem", g_dumpDir, g_exeName);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { Log("  (image dump: cannot create file, err %lu)", GetLastError()); return; }
    static uint8_t zero[0x10000];
    SIZE_T readable = 0;
    for (SIZE_T off = 0; off < size;) {
        MEMORY_BASIC_INFORMATION mbi;
        uint8_t* p = (uint8_t*)exe + off;
        SIZE_T chunk = 0x10000 - (off & 0xFFFF);
        if (chunk > size - off) chunk = size - off;
        const void* src = zero;
        if (VirtualQuery(p, &mbi, sizeof mbi)) {
            SIZE_T inRegion = (uint8_t*)mbi.BaseAddress + mbi.RegionSize - p;
            if (chunk > inRegion) chunk = inRegion;
            if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) { src = p; readable += chunk; }
        }
        DWORD w; WriteFile(h, src, (DWORD)chunk, &w, nullptr);
        off += chunk;
    }
    CloseHandle(h);
    Log("  Exe memory image: %s (%llu MB, %llu MB readable, base %p)", U8(path).b, (unsigned long long)(size >> 20), (unsigned long long)(readable >> 20), (void*)exe);
}

static void CrashReport(const char* kind, EXCEPTION_POINTERS* ep, bool full, bool dump) {
    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    CONTEXT* c = ep->ContextRecord;
    Log("==================== %s ====================", kind);
    Log("  Exception 0x%08lX %s at %s (flags 0x%lx)", er->ExceptionCode, ExcName(er->ExceptionCode), AddrName(er->ExceptionAddress), er->ExceptionFlags);
    if ((er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || er->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) && er->NumberParameters >= 2) {
        const char* op = er->ExceptionInformation[0] == 0 ? "reading" : er->ExceptionInformation[0] == 1 ? "writing" : "executing";
        Log("  -> %s address 0x%016llx%s", op, (unsigned long long)er->ExceptionInformation[1],
            er->ExceptionInformation[1] < 0x10000 ? "  (NULL pointer + offset)" : "");
    }
    Log("  RAX=%016llx RBX=%016llx RCX=%016llx RDX=%016llx", c->Rax, c->Rbx, c->Rcx, c->Rdx);
    Log("  RSI=%016llx RDI=%016llx RBP=%016llx RSP=%016llx", c->Rsi, c->Rdi, c->Rbp, c->Rsp);
    Log("  R8 =%016llx R9 =%016llx R10=%016llx R11=%016llx", c->R8, c->R9, c->R10, c->R11);
    Log("  R12=%016llx R13=%016llx R14=%016llx R15=%016llx", c->R12, c->R13, c->R14, c->R15);
    Log("  RIP=%016llx EFLAGS=%08lx", c->Rip, c->EFlags);
    if (Readable((void*)c->Rip, 16)) {
        const uint8_t* b = (const uint8_t*)c->Rip;
        Log("  Code at RIP: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
            b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    }
    WalkStack(c, 32);
    DescribeObjects(c);
    if (full && g_cfg.codeSnapshots) {
        static volatile LONG snapDone;
        if (!InterlockedExchange(&snapDone, 1)) {
            int n = 0;
            for (int i = 0; i < g_frameCount && i < 12; i++) SnapshotCode(g_frames[i], n++);
            for (int i = 0; i < 160 && n < 24; i++) {   // stack-scan code pointers too
                DWORD64* p = (DWORD64*)c->Rsp + i;
                if (!Readable(p, 8)) break;
                if (IsCodeAddr(*p)) SnapshotCode(*p, n++);
            }
            Log("  Code snapshots: %d files written to dumps\\code_*.bin", n);
            if (g_cfg.imageDump) DumpExeImage();
        }
    }
    if (full) {
        ScanStack(c->Rsp);
        LogCrumbs();
        Log("  Devices created: %ld, last device %p", g_deviceCount, (void*)g_lastDevice);
        LogModules();
    }
    if (dump) WriteMinidump(kind[0] == 'U' ? "crash" : "firstchance", ep);
    Log("==================== end of report ====================");
}

// RemapFallback via the exception handler (used when the game's code can't be patched):
// address of "mov rax,[r10+18]" in RemapTable_Find; a fault there with R10 == 0 means
// "remapped buffer not in table" -> return the original resource (RDX) to the caller.
static uint8_t* volatile g_remapFaultSite;
static volatile LONG g_remapFixups;

// Pragmata 1.1.0.0 layout (only used when the RemapFallback signature matched = same build)
#define PRAG_RENDERDEVICE_VTBL_RVA  0xa2b79a8ull   // render device object vtable
#define PRAG_DEV_CAPS_OFF           0xf38460       // 64-bit capability/option flags
#define PRAG_DEV_REMAPTABLE_OFF     0x8            // remap table pointer
static volatile LONG g_remapProbeDone;
#define PRAG_WORKER_GLOBAL_RVA      0xcfe6ea8ull   // render worker object pointer
#define PRAG_W_RELOC_LIST           0x43fb8        // worker: relocation list pointer
#define PRAG_W_RELOC_COUNT          0x43fc0        // worker: relocation list count
#define PRAG_W_RELOC_VALID          0x43fc8        // worker: relocation list valid (from init params +0x2a)
#define PRAG_W_FRAME                0x278          // worker: frame data (+0x30 gen, +0x34 count, +0x38 list)
#define PRAG_DEV_LAST_RELOC_GEN     0xf41a80       // device: generation of the last relocation pass
static uint8_t* volatile g_pragDevice;           // learned at the first remap fault
static bool PragKnownBuild() { return g_remapFaultSite != nullptr; }
static uint8_t* PragWorker() {
    uint8_t** g = (uint8_t**)((uintptr_t)GetModuleHandleW(nullptr) + PRAG_WORKER_GLOBAL_RVA);
    if (!Readable(g, 8)) return nullptr;
    uint8_t* w = *g;
    return (w && Readable(w + PRAG_W_RELOC_VALID, 2) && Readable(w + PRAG_W_FRAME, 0x40)) ? w : nullptr;
}
static void PragRelocProbe(const char* when) {
    if (!PragKnownBuild()) return;
    uint8_t* w = PragWorker();
    if (!w) { Log("[reloc] (%s) render worker not created yet", when); return; }
    uint8_t* f = w + PRAG_W_FRAME;
    char dev[160] = "";
    uint8_t* d = g_pragDevice;
    if (d && Readable(d + PRAG_DEV_LAST_RELOC_GEN, 4) && Readable(d + PRAG_DEV_CAPS_OFF, 8)) {
        uint8_t* tbl = *(uint8_t**)(d + PRAG_DEV_REMAPTABLE_OFF);
        snprintf(dev, sizeof dev, " | device lastGen=%u caps.bit32=%d table{counter=%u size=%llu}", *(uint32_t*)(d + PRAG_DEV_LAST_RELOC_GEN),
                 (int)((*(uint64_t*)(d + PRAG_DEV_CAPS_OFF) >> 32) & 1),
                 (tbl && Readable(tbl, 0x20)) ? *(uint32_t*)tbl : 0, (tbl && Readable(tbl, 0x20)) ? (unsigned long long)*(uint64_t*)(tbl + 0x18) : 0ull);
    }
    Log("[reloc] (%s) worker %p: listValid=%u byte43fc9=%u list=%p count=%u | frame gen=%u count=%u list=%p%s", when, (void*)w,
        w[PRAG_W_RELOC_VALID], w[PRAG_W_RELOC_VALID + 1], *(void**)(w + PRAG_W_RELOC_LIST), *(uint32_t*)(w + PRAG_W_RELOC_COUNT),
        *(uint32_t*)(f + 0x30), *(uint32_t*)(f + 0x34), *(void**)(f + 0x38), dev);
}
static DWORD WINAPI ForceRelocListThread(void*) {
    // wait for the game's code to be readable and the render worker to exist, then keep the flag on
    for (int i = 0; i < 300 && !PragKnownBuild(); i++) Sleep(10);
    if (!PragKnownBuild()) { Log("[patch] ForceRelocList: unknown game build - not applied"); return 0; }
    int sets = 0;
    for (int ms = 0; ms < 120000; ms += 2) {
        uint8_t* w = PragWorker();
        if (w && w[PRAG_W_RELOC_VALID] == 0) {
            w[PRAG_W_RELOC_VALID] = 1;
            if (++sets <= 5) Log("[patch] ForceRelocList: render worker %p relocation list was off -> set on (#%d)", (void*)w, sets);
        }
        Sleep(2);
    }
    Log("[patch] ForceRelocList: watch ended (%d set(s))", sets);
    return 0;
}

static void RemapProbe(CONTEXT* c) {
    if (InterlockedExchange(&g_remapProbeDone, 1)) return;
    uintptr_t vt = (uintptr_t)GetModuleHandleW(nullptr) + PRAG_RENDERDEVICE_VTBL_RVA;
    const DWORD64 regs[] = { c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->R8, c->R9, c->R10, c->R11, c->R12, c->R13, c->R14, c->R15 };
    uint8_t* dev = nullptr;
    for (DWORD64 r : regs) if (r > 0x10000 && Readable((void*)r, 8) && *(uintptr_t*)r == vt) { dev = (uint8_t*)r; break; }
    if (!dev) {   // the buffer object (RBX/RSI) points to its owner (= device) at +0x108
        for (DWORD64 r : regs) if (r > 0x10000 && Readable((void*)(r + 0x108), 8)) {
            uint8_t* o = *(uint8_t**)(r + 0x108);
            if (o && Readable(o, 8) && *(uintptr_t*)o == vt) { dev = o; break; }
        }
    }
    if (!dev || !Readable(dev + PRAG_DEV_CAPS_OFF, 8)) { Log("[probe] render device object not found in registers"); return; }
    g_pragDevice = dev;
    uint64_t caps = *(uint64_t*)(dev + PRAG_DEV_CAPS_OFF);
    Log("[probe] render device %p caps=0x%016llx  bit15=%d bit19(FL>11_0)=%d bit26=%d bit32(relocation)=%d bit34=%d",
        (void*)dev, (unsigned long long)caps, (int)((caps >> 15) & 1), (int)((caps >> 19) & 1), (int)((caps >> 26) & 1), (int)((caps >> 32) & 1), (int)((caps >> 34) & 1));
    uint8_t* tbl = *(uint8_t**)(dev + PRAG_DEV_REMAPTABLE_OFF);
    if (tbl && Readable(tbl, 0x40))
        Log("[probe] remap table %p: counter=%u enabled=%u buckets=%llu size=%llu", (void*)tbl, *(uint32_t*)tbl, tbl[4],
            (unsigned long long)*(uint64_t*)(tbl + 0x10), (unsigned long long)*(uint64_t*)(tbl + 0x18));
    if (g_cfg.forceRelocation && !((caps >> 32) & 1)) {
        InterlockedOr64((volatile LONG64*)(dev + PRAG_DEV_CAPS_OFF), (LONG64)(1ull << 32));
        Log("[patch] ForceRelocation: set relocation bit 32 in the render device flags (was off) - the game's relocation pass can now fill the remap table");
    } else if (g_cfg.forceRelocation) Log("[patch] ForceRelocation: bit 32 already set - nothing to change");
    PragRelocProbe("first remap fault");
}

static LONG CALLBACK Veh(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION && g_remapFaultSite && ep->ExceptionRecord->ExceptionAddress == (void*)g_remapFaultSite) {
        CONTEXT* c = ep->ContextRecord;
        if (c->R10 == 0 && Readable((void*)c->Rsp, 8)) {
            if (!g_remapProbeDone) { TlsSet(TLS_IN_VEH, true); RemapProbe(c); TlsSet(TLS_IN_VEH, false); }
            c->Rax = c->Rdx;                       // mov rax, rdx
            c->Rip = *(DWORD64*)c->Rsp;            // ret
            c->Rsp += 8;
            LONG n = InterlockedIncrement(&g_remapFixups);
            static void* seen[24]; static volatile LONG nseen;
            bool isNew = nseen < 24;      // only the first 24 distinct buffers are reported individually
            for (LONG i = 0; i < nseen && i < 24; i++) if (seen[i] == (void*)c->Rdx) isNew = false;
            if (isNew || n <= 5 || (n & (n - 1)) == 0) {    // new buffers, 1..5, then powers of two
                DWORD err = GetLastError();
                TlsSet(TLS_IN_VEH, true);
                Log("[fix] RemapFallback #%ld: remapped buffer %p not in table, using it directly (returning to %s)", n, (void*)c->Rdx, AddrName((void*)c->Rip));
                if (isNew) { LONG k = InterlockedIncrement(&nseen) - 1; if (k < 24) { seen[k] = (void*)c->Rdx; DescribeValue("remapped", c->Rdx); } }
                TlsSet(TLS_IN_VEH, false);
                SetLastError(err);
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    uintptr_t fl = TlsFlags();

    // Game / debug-layer OutputDebugString output
    if (code == 0x40010006 /*DBG_PRINTEXCEPTION_C*/) {
        if (g_cfg.captureDebugOutput && !(fl & (TLS_IN_LOG | TLS_IN_VEH)) && ep->ExceptionRecord->NumberParameters >= 2 &&
            InterlockedIncrement(&g_debugLines) <= g_cfg.maxDebugOutputLines) {
            const char* s = (const char*)ep->ExceptionRecord->ExceptionInformation[1];
            size_t n = (size_t)ep->ExceptionRecord->ExceptionInformation[0];
            if (s && n && Readable(s, n)) {
                char line[1024]; size_t k = n < sizeof line - 1 ? n : sizeof line - 1;
                memcpy(line, s, k); line[k] = 0;
                while (k && (line[k - 1] == '\n' || line[k - 1] == '\r' || line[k - 1] == 0)) line[--k] = 0;
                TlsSet(TLS_IN_VEH, true);
                if (k) Log("[debug-output] %s", line);
                TlsSet(TLS_IN_VEH, false);
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (!IsInteresting(code) || (fl & TLS_IN_VEH)) return EXCEPTION_CONTINUE_SEARCH;
    DWORD err = GetLastError();
    TlsSet(TLS_IN_VEH, true);

    if (code == EXCEPTION_STACK_OVERFLOW) {
        // very little stack left: no formatting
        static const char msg[] = "!! STACK_OVERFLOW exception (first chance) - the game is about to crash\r\n";
        EnterCriticalSection(&g_logCs); LogWrite(msg, sizeof msg - 1); LeaveCriticalSection(&g_logCs);
    } else {
        LONG n = InterlockedIncrement(&g_firstChance);
        if (n <= g_cfg.maxFirstChanceReports) {
            CrashReport("FIRST-CHANCE EXCEPTION (may still be handled by the game)", ep, n == 1, n <= g_cfg.firstChanceDumps);
        } else if (n <= g_cfg.maxFirstChanceLines) {
            EXCEPTION_RECORD* er = ep->ExceptionRecord;
            Log("[first-chance] 0x%08lX %s at %s  info=[%llx, %llx]", code, ExcName(code), AddrName(er->ExceptionAddress),
                er->NumberParameters > 0 ? (unsigned long long)er->ExceptionInformation[0] : 0ull,
                er->NumberParameters > 1 ? (unsigned long long)er->ExceptionInformation[1] : 0ull);
        } else if (n == g_cfg.maxFirstChanceLines + 1) {
            Log("[first-chance] limit reached, not logging further first-chance exceptions");
        }
    }
    TlsSet(TLS_IN_VEH, false);
    SetLastError(err);
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI Uef(EXCEPTION_POINTERS* ep) {
    static volatile LONG count;
    LONG n = InterlockedIncrement(&count);
    TlsSet(TLS_IN_VEH, true);
    if (n == 1) CrashReport("UNHANDLED EXCEPTION - GAME CRASH", ep, true, true);
    else if (n <= 5) Log("[unhandled again #%ld] 0x%08lX %s at %s", n, ep->ExceptionRecord->ExceptionCode, ExcName(ep->ExceptionRecord->ExceptionCode), AddrName(ep->ExceptionRecord->ExceptionAddress));
    TlsSet(TLS_IN_VEH, false);
    LPTOP_LEVEL_EXCEPTION_FILTER f = g_gameFilter;
    if (f && f != Uef) { Log("Passing the crash on to the game's own crash handler at %s", AddrName((void*)f)); return f(ep); }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Replacement for SetUnhandledExceptionFilter: remember the game's filter and
// keep ours installed, so the report is always written (game's filter still runs).
static LPTOP_LEVEL_EXCEPTION_FILTER WINAPI My_SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f) {
    LPTOP_LEVEL_EXCEPTION_FILTER prev = g_gameFilter;
    g_gameFilter = (f == Uef) ? nullptr : f;
    Log("Game called SetUnhandledExceptionFilter(%s) - chained behind the proxy's crash handler", f ? AddrName((void*)f) : "NULL");
    return prev;
}

static void InstallExceptionHandlers() {
    AddVectoredExceptionHandler(1, Veh);
    g_gameFilter = SetUnhandledExceptionFilter(Uef);
    if (!g_cfg.keepFilterOnTop) return;
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    uint8_t* fn = kb ? (uint8_t*)(void*)GetProcAddress(kb, "SetUnhandledExceptionFilter") : nullptr;
    if (!fn) return;
    uint8_t patch[12] = { 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0 }; // mov rax, imm64 ; jmp rax
    void* target = (void*)&My_SetUnhandledExceptionFilter;
    memcpy(patch + 2, &target, 8);
    DWORD old;
    if (VirtualProtect(fn, sizeof patch, PAGE_EXECUTE_READWRITE, &old)) {
        memcpy(fn, patch, sizeof patch);
        VirtualProtect(fn, sizeof patch, old, &old);
        FlushInstructionCache(GetCurrentProcess(), fn, sizeof patch);
        g_filterPatched = true;
    }
    Log("Crash handlers installed (vectored + unhandled filter%s)", g_filterPatched ? ", kept on top of the game's" : "");
}

// =============================================================================
//  Feature-check logging
// =============================================================================
static void DescribeFeature(D3D12_FEATURE f, const void* d, UINT sz, char* o, size_t n) {
#define FITS(T) (sz >= sizeof(T))
    o[0] = 0;
    switch ((int)f) {
    case D3D12_FEATURE_D3D12_OPTIONS: if (FITS(D3D12_FEATURE_DATA_D3D12_OPTIONS)) { auto* x = (const D3D12_FEATURE_DATA_D3D12_OPTIONS*)d;
        snprintf(o, n, "ResourceBindingTier=%d TiledResourcesTier=%d ResourceHeapTier=%d TypedUAVLoadAdditionalFormats=%d ROVs=%d ConservativeRasterTier=%d Doubles=%d MinPrecision=%d PSStencilRef=%d VPAndRTArrayIndexAnyShader=%d MaxGPUVABitsPerResource=%u",
                 x->ResourceBindingTier, x->TiledResourcesTier, x->ResourceHeapTier, x->TypedUAVLoadAdditionalFormats, x->ROVsSupported,
                 x->ConservativeRasterizationTier, x->DoublePrecisionFloatShaderOps, x->MinPrecisionSupport, x->PSSpecifiedStencilRefSupported,
                 x->VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation, x->MaxGPUVirtualAddressBitsPerResource); } break;
    case D3D12_FEATURE_ARCHITECTURE: if (FITS(D3D12_FEATURE_DATA_ARCHITECTURE)) { auto* x = (const D3D12_FEATURE_DATA_ARCHITECTURE*)d;
        snprintf(o, n, "Node=%u TileBased=%d UMA=%d CacheCoherentUMA=%d", x->NodeIndex, x->TileBasedRenderer, x->UMA, x->CacheCoherentUMA); } break;
    case D3D12_FEATURE_ARCHITECTURE1: if (FITS(D3D12_FEATURE_DATA_ARCHITECTURE1)) { auto* x = (const D3D12_FEATURE_DATA_ARCHITECTURE1*)d;
        snprintf(o, n, "Node=%u TileBased=%d UMA=%d CacheCoherentUMA=%d IsolatedMMU=%d", x->NodeIndex, x->TileBasedRenderer, x->UMA, x->CacheCoherentUMA, x->IsolatedMMU); } break;
    case D3D12_FEATURE_FEATURE_LEVELS: if (FITS(D3D12_FEATURE_DATA_FEATURE_LEVELS)) { auto* x = (const D3D12_FEATURE_DATA_FEATURE_LEVELS*)d;
        size_t k = snprintf(o, n, "requested {");
        for (UINT i = 0; x->pFeatureLevelsRequested && i < x->NumFeatureLevels && i < 12 && k < n; i++) k += snprintf(o + k, n - k, "%s%s", i ? "," : "", FlName(x->pFeatureLevelsRequested[i]));
        if (k < n) snprintf(o + k, n - k, "} -> MaxSupportedFeatureLevel=%s", FlName(x->MaxSupportedFeatureLevel)); } break;
    case D3D12_FEATURE_FORMAT_SUPPORT: if (FITS(D3D12_FEATURE_DATA_FORMAT_SUPPORT)) { auto* x = (const D3D12_FEATURE_DATA_FORMAT_SUPPORT*)d;
        snprintf(o, n, "%s Support1=0x%08x Support2=0x%08x", FormatName(x->Format), (unsigned)x->Support1, (unsigned)x->Support2); } break;
    case D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS: if (FITS(D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS)) { auto* x = (const D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS*)d;
        snprintf(o, n, "%s x%u flags=%d -> NumQualityLevels=%u", FormatName(x->Format), x->SampleCount, x->Flags, x->NumQualityLevels); } break;
    case D3D12_FEATURE_FORMAT_INFO: if (FITS(D3D12_FEATURE_DATA_FORMAT_INFO)) { auto* x = (const D3D12_FEATURE_DATA_FORMAT_INFO*)d;
        snprintf(o, n, "%s PlaneCount=%u", FormatName(x->Format), x->PlaneCount); } break;
    case D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT: if (FITS(D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT)) { auto* x = (const D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT*)d;
        snprintf(o, n, "BitsPerResource=%u BitsPerProcess=%u", x->MaxGPUVirtualAddressBitsPerResource, x->MaxGPUVirtualAddressBitsPerProcess); } break;
    case D3D12_FEATURE_SHADER_MODEL: if (FITS(D3D12_FEATURE_DATA_SHADER_MODEL)) { auto* x = (const D3D12_FEATURE_DATA_SHADER_MODEL*)d;
        snprintf(o, n, "HighestShaderModel=%x.%x", (x->HighestShaderModel >> 4) & 0xF, x->HighestShaderModel & 0xF); } break;
    case D3D12_FEATURE_D3D12_OPTIONS1: if (FITS(D3D12_FEATURE_DATA_D3D12_OPTIONS1)) { auto* x = (const D3D12_FEATURE_DATA_D3D12_OPTIONS1*)d;
        snprintf(o, n, "WaveOps=%d WaveLanes=%u..%u TotalLanes=%u ExpandedComputeResourceStates=%d Int64ShaderOps=%d", x->WaveOps, x->WaveLaneCountMin, x->WaveLaneCountMax, x->TotalLaneCount, x->ExpandedComputeResourceStates, x->Int64ShaderOps); } break;
    case D3D12_FEATURE_ROOT_SIGNATURE: if (FITS(D3D12_FEATURE_DATA_ROOT_SIGNATURE)) { auto* x = (const D3D12_FEATURE_DATA_ROOT_SIGNATURE*)d;
        snprintf(o, n, "HighestVersion=0x%x", x->HighestVersion); } break;
    case D3D12_FEATURE_D3D12_OPTIONS2: if (FITS(D3D12_FEATURE_DATA_D3D12_OPTIONS2)) { auto* x = (const D3D12_FEATURE_DATA_D3D12_OPTIONS2*)d;
        snprintf(o, n, "DepthBoundsTest=%d ProgrammableSamplePositionsTier=%d", x->DepthBoundsTestSupported, x->ProgrammableSamplePositionsTier); } break;
    case D3D12_FEATURE_SHADER_CACHE: if (FITS(D3D12_FEATURE_DATA_SHADER_CACHE)) { auto* x = (const D3D12_FEATURE_DATA_SHADER_CACHE*)d;
        snprintf(o, n, "SupportFlags=0x%x", x->SupportFlags); } break;
    case D3D12_FEATURE_COMMAND_QUEUE_PRIORITY: if (FITS(D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY)) { auto* x = (const D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY*)d;
        snprintf(o, n, "ListType=%d Priority=%u -> Supported=%d", x->CommandListType, x->Priority, x->PriorityForTypeIsSupported); } break;
    case D3D12_FEATURE_D3D12_OPTIONS3: if (FITS(D3D12_FEATURE_DATA_D3D12_OPTIONS3)) { auto* x = (const D3D12_FEATURE_DATA_D3D12_OPTIONS3*)d;
        snprintf(o, n, "CopyQueueTimestamps=%d CastingFullyTypedFormat=%d WriteBufferImmediate=0x%x ViewInstancingTier=%d Barycentrics=%d", x->CopyQueueTimestampQueriesSupported, x->CastingFullyTypedFormatSupported, x->WriteBufferImmediateSupportFlags, x->ViewInstancingTier, x->BarycentricsSupported); } break;
    case D3D12_FEATURE_D3D12_OPTIONS4: if (FITS(D3D12_FEATURE_DATA_D3D12_OPTIONS4)) { auto* x = (const D3D12_FEATURE_DATA_D3D12_OPTIONS4*)d;
        snprintf(o, n, "MSAA64KBAligned=%d SharedResourceCompatTier=%d Native16BitShaderOps=%d", x->MSAA64KBAlignedTextureSupported, x->SharedResourceCompatibilityTier, x->Native16BitShaderOpsSupported); } break;
    case D3D12_FEATURE_D3D12_OPTIONS5: if (FITS(D3D12_FEATURE_DATA_D3D12_OPTIONS5)) { auto* x = (const D3D12_FEATURE_DATA_D3D12_OPTIONS5*)d;
        snprintf(o, n, "SRVOnlyTiledTier3=%d RenderPassesTier=%d RaytracingTier=0x%x", x->SRVOnlyTiledResourceTier3, x->RenderPassesTier, x->RaytracingTier); } break;
    case D3D12_FEATURE_D3D12_OPTIONS6: if (FITS(D3D12_FEATURE_DATA_D3D12_OPTIONS6)) { auto* x = (const D3D12_FEATURE_DATA_D3D12_OPTIONS6*)d;
        snprintf(o, n, "AdditionalShadingRates=%d VariableShadingRateTier=%d ShadingRateImageTileSize=%u BackgroundProcessing=%d", x->AdditionalShadingRatesSupported, x->VariableShadingRateTier, x->ShadingRateImageTileSize, x->BackgroundProcessingSupported); } break;
    case D3D12_FEATURE_D3D12_OPTIONS7: if (FITS(D3D12_FEATURE_DATA_D3D12_OPTIONS7)) { auto* x = (const D3D12_FEATURE_DATA_D3D12_OPTIONS7*)d;
        snprintf(o, n, "MeshShaderTier=%d SamplerFeedbackTier=%d", x->MeshShaderTier, x->SamplerFeedbackTier); } break;
    case D3D12_FEATURE_D3D12_OPTIONS9: if (FITS(D3D12_FEATURE_DATA_D3D12_OPTIONS9)) { auto* x = (const D3D12_FEATURE_DATA_D3D12_OPTIONS9*)d;
        snprintf(o, n, "MeshShaderPipelineStats=%d AtomicInt64Typed=%d AtomicInt64GroupShared=%d DerivativesInMeshAmp=%d WaveMMATier=%d", x->MeshShaderPipelineStatsSupported, x->AtomicInt64OnTypedResourceSupported, x->AtomicInt64OnGroupSharedSupported, x->DerivativesInMeshAndAmplificationShadersSupported, x->WaveMMATier); } break;
    case D3D12_FEATURE_D3D12_OPTIONS12: if (FITS(D3D12_FEATURE_DATA_D3D12_OPTIONS12)) { auto* x = (const D3D12_FEATURE_DATA_D3D12_OPTIONS12*)d;
        snprintf(o, n, "EnhancedBarriers=%d RelaxedFormatCasting=%d", x->EnhancedBarriersSupported, x->RelaxedFormatCastingSupported); } break;
    case D3D12_FEATURE_D3D12_TIGHT_ALIGNMENT: if (FITS(D3D12_FEATURE_DATA_TIGHT_ALIGNMENT)) { auto* x = (const D3D12_FEATURE_DATA_TIGHT_ALIGNMENT*)d;
        snprintf(o, n, "SupportTier=%d (0=not supported, 1=tier 1)", x->SupportTier); } break;
    case D3D12_FEATURE_D3D12_OPTIONS16: if (FITS(D3D12_FEATURE_DATA_D3D12_OPTIONS16)) { auto* x = (const D3D12_FEATURE_DATA_D3D12_OPTIONS16*)d;
        snprintf(o, n, "DynamicDepthBias=%d GPUUploadHeap=%d", x->DynamicDepthBiasSupported, x->GPUUploadHeapSupported); } break;
    }
    if (!o[0]) snprintf(o, n, "(%u bytes)", sz);
#undef FITS
}

// de-duplication of feature-check log lines
static uint64_t g_featSeen[2048];
static bool FeatureSeen(uint64_t key) {
    key |= 1;
    uint32_t i = (uint32_t)(key * 0x9E3779B97F4A7C15ull >> 53);
    for (int probe = 0; probe < 2048; probe++, i = (i + 1) & 2047) {
        uint64_t cur = g_featSeen[i];
        if (cur == key) return true;
        if (!cur) {
            if (InterlockedCompareExchange64((volatile LONG64*)&g_featSeen[i], (LONG64)key, 0) == 0) return false;
            if (g_featSeen[i] == key) return true;
        }
    }
    return true;
}

// =============================================================================
//  Shader inspection: DXBC/DXIL container -> shader model, stage, required features
// =============================================================================
static void DescribeShader(const void* code, size_t len, char* o, size_t n) {
    o[0] = 0;
    if (!code || len < 32 || !Readable(code, len)) { snprintf(o, n, "none"); return; }
    const uint8_t* b = (const uint8_t*)code;
    if (memcmp(b, "DXBC", 4)) { snprintf(o, n, "%zu bytes, not a DXBC container", len); return; }
    uint32_t parts = *(const uint32_t*)(b + 28);
    const char* kind = "?"; int major = 0, minor = 0; bool dxil = false; uint64_t sfi0 = 0; bool hasSfi = false;
    static const char* kinds[] = { "ps", "vs", "gs", "hs", "ds", "cs", "lib", "raygen", "isect", "anyhit", "closesthit", "miss", "callable", "ms", "as", "node" };
    for (uint32_t i = 0; i < parts && 32 + i * 4 + 4 <= len; i++) {
        uint32_t off = *(const uint32_t*)(b + 32 + i * 4);
        if (off + 8 > len) continue;
        const uint8_t* p = b + off; uint32_t psz = *(const uint32_t*)(p + 4);
        if (off + 8 + psz > len) continue;
        if (!memcmp(p, "DXIL", 4) || !memcmp(p, "SHEX", 4) || !memcmp(p, "SHDR", 4)) {
            if (psz >= 4) {
                uint32_t ver = *(const uint32_t*)(p + 8);
                minor = ver & 0xF; major = (ver >> 4) & 0xF; uint32_t k = ver >> 16;
                kind = k < 16 ? kinds[k] : "?";
                dxil = !memcmp(p, "DXIL", 4);
            }
        } else if (!memcmp(p, "SFI0", 4) && psz >= 8) {
            sfi0 = *(const uint64_t*)(p + 8); hasSfi = true;
        }
    }
    size_t k = snprintf(o, n, "%s %s_%d_%d, %zu bytes", dxil ? "DXIL" : "DXBC", kind, major, minor, len);
    if (hasSfi && k < n) {
        static const struct { uint64_t bit; const char* name; } flags[] = {
            {0x1, "Doubles"}, {0x2, "RawStructuredBufs(4.x)"}, {0x4, "UAVsAtEveryStage"}, {0x8, "64UAVs(needs 11_1)"},
            {0x10, "MinPrecision"}, {0x20, "11_1_DoubleExt"}, {0x40, "11_1_ShaderExt"}, {0x80, "L9ComparisonFilter"},
            {0x100, "TiledResources"}, {0x200, "StencilRef"}, {0x400, "InnerCoverage"}, {0x800, "TypedUAVLoadAdditionalFormats"},
            {0x1000, "ROVs"}, {0x2000, "VPAndRTArrayIndexAnyShader"}, {0x4000, "WaveOps"}, {0x8000, "Int64Ops"},
            {0x10000, "ViewID"}, {0x20000, "Barycentrics"}, {0x40000, "Native16Bit"}, {0x80000, "ShadingRate"},
            {0x100000, "Raytracing1.1"}, {0x200000, "SamplerFeedback"}, {0x400000, "AtomicInt64Typed"}, {0x800000, "AtomicInt64GroupShared"},
            {0x1000000, "DerivativesInMeshAmp"}, {0x2000000, "ResourceDescriptorHeapIndexing(SM6.6)"}, {0x4000000, "SamplerDescriptorHeapIndexing(SM6.6)"},
            {0x10000000, "AtomicInt64OnHeapResource"}, {0x20000000, "AdvancedTextureOps"}, {0x40000000, "WriteableMSAATextures"},
            {0x80000000ull, "SampleCmpGradientOrBias"}, {0x100000000ull, "ExtendedCommandInfo"} };
        k += snprintf(o + k, n - k, ", SFI0=0x%llx", (unsigned long long)sfi0);
        for (auto& f : flags) if ((sfi0 & f.bit) && k < n) k += snprintf(o + k, n - k, " %s", f.name);
    }
}

static void DumpBlob(const char* what, const char* stage, const void* code, size_t len) {
    if (!g_cfg.dumpFailedShaders || !code || !len || !Readable(code, len)) return;
    if (InterlockedIncrement(&g_shaderDumps) > 300) return;
    wchar_t path[MAX_PATH];
    wchar_t ww[32], ws[16];
    MultiByteToWideChar(CP_UTF8, 0, what, -1, ww, 32); MultiByteToWideChar(CP_UTF8, 0, stage, -1, ws, 16);
    swprintf(path, MAX_PATH, L"%ls\\%ls_%ls_%016llx.bin", g_dumpDir, ww, ws, (unsigned long long)Fnv1a(code, len));
    if (!FileExists(path)) WriteWholeFile(path, code, len);
}

static void LogShader(const char* what, const char* stage, const D3D12_SHADER_BYTECODE& bc) {
    if (!bc.pShaderBytecode || !bc.BytecodeLength) return;
    char d[512]; DescribeShader(bc.pShaderBytecode, bc.BytecodeLength, d, sizeof d);
    Log("    %s: %s", stage, d);
    DumpBlob(what, stage, bc.pShaderBytecode, bc.BytecodeLength);
}

// =============================================================================
//  Device vtable hooks
// =============================================================================
#define DECL_HOOK(name, ret, ...) typedef ret (STDMETHODCALLTYPE *PFN_##name)(__VA_ARGS__); static PFN_##name o_##name;

DECL_HOOK(CreateCommandQueue, HRESULT, ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**)
DECL_HOOK(CreateCommandAllocator, HRESULT, ID3D12Device*, D3D12_COMMAND_LIST_TYPE, REFIID, void**)
DECL_HOOK(CreateGraphicsPipelineState, HRESULT, ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**)
DECL_HOOK(CreateComputePipelineState, HRESULT, ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**)
DECL_HOOK(CreateCommandList, HRESULT, ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**)
DECL_HOOK(CheckFeatureSupport, HRESULT, ID3D12Device*, D3D12_FEATURE, void*, UINT)
DECL_HOOK(CreateDescriptorHeap, HRESULT, ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**)
DECL_HOOK(CreateRootSignature, HRESULT, ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**)
DECL_HOOK(CreateCommittedResource, HRESULT, ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**)
DECL_HOOK(CreateHeap, HRESULT, ID3D12Device*, const D3D12_HEAP_DESC*, REFIID, void**)
DECL_HOOK(CreatePlacedResource, HRESULT, ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**)
DECL_HOOK(CreateReservedResource, HRESULT, ID3D12Device*, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**)
DECL_HOOK(CreateFence, HRESULT, ID3D12Device*, UINT64, D3D12_FENCE_FLAGS, REFIID, void**)
DECL_HOOK(CreateRenderTargetView, void, ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE)
DECL_HOOK(CreateDepthStencilView, void, ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE)
DECL_HOOK(CopyDescriptors, void, ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE)
DECL_HOOK(CopyDescriptorsSimple, void, ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE)
DECL_HOOK(GetDeviceRemovedReason, HRESULT, ID3D12Device*)
DECL_HOOK(CreateQueryHeap, HRESULT, ID3D12Device*, const D3D12_QUERY_HEAP_DESC*, REFIID, void**)
DECL_HOOK(CreateCommandSignature, HRESULT, ID3D12Device*, const D3D12_COMMAND_SIGNATURE_DESC*, ID3D12RootSignature*, REFIID, void**)
DECL_HOOK(CreatePipelineState, HRESULT, ID3D12Device*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**)
DECL_HOOK(CreateCommittedResource1, HRESULT, ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**)
DECL_HOOK(CreateCommandList1, HRESULT, ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS, REFIID, void**)
DECL_HOOK(CreateHeap1, HRESULT, ID3D12Device*, const D3D12_HEAP_DESC*, ID3D12ProtectedResourceSession*, REFIID, void**)
DECL_HOOK(CreateStateObject, HRESULT, ID3D12Device*, const D3D12_STATE_OBJECT_DESC*, REFIID, void**)
DECL_HOOK(CreateCommittedResource2, HRESULT, ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**)
DECL_HOOK(CreatePlacedResource1, HRESULT, ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**)
DECL_HOOK(CreateCommittedResource3, HRESULT, ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC1*, D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, UINT32, const DXGI_FORMAT*, REFIID, void**)
DECL_HOOK(CreateCommandQueue1, HRESULT, ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, REFIID, void**)
DECL_HOOK(CreatePlacedResource2, HRESULT, ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*, D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*, UINT32, const DXGI_FORMAT*, REFIID, void**)

static bool FailBudget() { return InterlockedIncrement(&g_failLines) <= 2000; }

// ---- heap registry: heap pointer -> type/flags (so placed resources show their heap type)
struct HeapRec { void* volatile key; int type; UINT flags; UINT64 size; };
static HeapRec g_heaps[4096];
static HeapRec* HeapSlot(void* p, bool insert) {
    if (!p) return nullptr;
    uint32_t i = (uint32_t)((((uintptr_t)p >> 4) * 0x9E3779B97F4A7C15ull) >> 52);
    for (int n = 0; n < 64; n++, i = (i + 1) & 4095) {
        void* k = g_heaps[i].key;
        if (k == p) return &g_heaps[i];
        if (!k) {
            if (!insert) return nullptr;
            if (InterlockedCompareExchangePointer(&g_heaps[i].key, p, nullptr) == nullptr) return &g_heaps[i];
            if (g_heaps[i].key == p) return &g_heaps[i];
        }
    }
    return nullptr;
}
static void RecordHeap(void** pp, const D3D12_HEAP_DESC* d) {
    if (!pp || !*pp || !d) return;
    ID3D12Heap* h = nullptr;
    void* keys[2] = { *pp, nullptr };
    if (SUCCEEDED(((IUnknown*)*pp)->QueryInterface(IID_ID3D12Heap, (void**)&h)) && h) { keys[1] = h; h->Release(); }
    for (void* k : keys) if (HeapRec* r = HeapSlot(k, true)) { r->type = d->Properties.Type; r->flags = d->Flags; r->size = d->SizeInBytes; }
}
static void HookQueue(void* pq);
static const D3D12_HEAP_PROPERTIES* PlacedHeapProps(ID3D12Heap* h, int* flagsOut) {
    HeapRec* r = HeapSlot(h, false);
    if (!r) return nullptr;
    *flagsOut = r->flags;
    static const D3D12_HEAP_PROPERTIES kTypes[6] = { {(D3D12_HEAP_TYPE)0}, {D3D12_HEAP_TYPE_DEFAULT}, {D3D12_HEAP_TYPE_UPLOAD}, {D3D12_HEAP_TYPE_READBACK}, {D3D12_HEAP_TYPE_CUSTOM}, {(D3D12_HEAP_TYPE)5} };
    return (r->type >= 1 && r->type <= 5) ? &kTypes[r->type] : nullptr;
}

// ---- command queues
struct QueueRec { void* q; int type; };
static QueueRec g_queues[32]; static volatile LONG g_queueCount;
static volatile LONG c_exec[4], c_signal, c_wait, c_tile, c_removeDevice;
static int QueueType(void* q) { for (LONG i = 0; i < g_queueCount && i < 32; i++) if (g_queues[i].q == q) return g_queues[i].type; return -1; }
static const char* QTypeName(int t) { switch (t) { case 0: return "DIRECT"; case 1: return "BUNDLE"; case 2: return "COMPUTE"; case 3: return "COPY"; } return "?"; }

static const char* HeapTypeName(int t) {
    switch (t) { case 1: return "DEFAULT"; case 2: return "UPLOAD"; case 3: return "READBACK"; case 4: return "CUSTOM"; case 5: return "GPU_UPLOAD"; }
    return "?";
}
static const char* DimName(int d) {
    switch (d) { case 1: return "BUFFER"; case 2: return "TEX1D"; case 3: return "TEX2D"; case 4: return "TEX3D"; }
    return "UNKNOWN";
}
static void LogResDesc(const char* fn, HRESULT hr, const D3D12_HEAP_PROPERTIES* hp, int heapFlags, const D3D12_RESOURCE_DESC* d) {
    InterlockedIncrement(&c_fail);
    if (!FailBudget()) return;
    if (!d || !Readable(d, sizeof *d)) { Log("!! %s FAILED %s (desc=NULL)", fn, HrName(hr)); return; }
    Log("!! %s FAILED %s  heap=%s heapFlags=0x%x  %s %llux%ux%u mips=%u %s samples=%u layout=%d flags=0x%x",
        fn, HrName(hr), hp ? HeapTypeName(hp->Type) : "-", heapFlags, DimName(d->Dimension), (unsigned long long)d->Width, d->Height,
        d->DepthOrArraySize, d->MipLevels, FormatName(d->Format), d->SampleDesc.Count, d->Layout, d->Flags);
}

static void RecordRes(void* p, const char* fn, const D3D12_HEAP_PROPERTIES* hp, int hf, const D3D12_RESOURCE_DESC* d) {
    ResRec* r = ResSlot(p, true);
    if (!r) return;
    r->fn = fn; r->tid = GetCurrentThreadId(); r->t = NowSec();
    r->width = d->Width; r->height = d->Height; r->depth = d->DepthOrArraySize; r->mips = d->MipLevels;
    r->fmt = d->Format; r->dim = d->Dimension; r->layout = d->Layout; r->flags = d->Flags;
    r->heapType = hp ? hp->Type : -1; r->heapFlags = hf;
}
static void TrackRes(const char* fn, HRESULT hr, const D3D12_HEAP_PROPERTIES* hp, int hf, const D3D12_RESOURCE_DESC* d, void** pp) {
    if (FAILED(hr)) { LogResDesc(fn, hr, hp, hf, d); return; }
    if (!g_cfg.trackResources || !pp || !*pp || !d || !Readable(d, sizeof *d)) return;
    if (!g_res) {
        ResRec* mem = (ResRec*)VirtualAlloc(nullptr, sizeof(ResRec) * RES_SLOTS, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (InterlockedCompareExchangePointer((void* volatile*)&g_res, mem, nullptr) != nullptr && mem) VirtualFree(mem, 0, MEM_RELEASE);
    }
    RecordRes(*pp, fn, hp, hf, d);
    if (d->Flags & D3D12_RESOURCE_FLAG_USE_TIGHT_ALIGNMENT) {
        static volatile LONG n;
        if (InterlockedIncrement(&n) <= 40)
            Log("%s with USE_TIGHT_ALIGNMENT -> %p: %s %llux%u flags=0x%x heap=%s", fn, *pp, DimName(d->Dimension), (unsigned long long)d->Width, d->Height, d->Flags, hp ? HeapTypeName(hp->Type) : "(placed/reserved)");
    }
    IUnknown* u = (IUnknown*)*pp; ID3D12Resource* r = nullptr;
    if (SUCCEEDED(u->QueryInterface(IID_ID3D12Resource, (void**)&r)) && r) {
        if ((void*)r != *pp) RecordRes(r, fn, hp, hf, d);
        if (!g_resVtbl) { g_resVtbl = *(void**)r; Log("Resource object vtable = %s", AddrName(g_resVtbl)); }
        r->Release();
    }
}

static HRESULT STDMETHODCALLTYPE H_CreateCommandQueue(ID3D12Device* t, const D3D12_COMMAND_QUEUE_DESC* d, REFIID r, void** pp) {
    HRESULT hr = o_CreateCommandQueue(t, d, r, pp);
    AddCrumb("ID3D12Device::CreateCommandQueue", hr);
    if (SUCCEEDED(hr) && pp && *pp && d) {
        ID3D12CommandQueue* q = nullptr;
        if (SUCCEEDED(((IUnknown*)*pp)->QueryInterface(IID_ID3D12CommandQueue, (void**)&q)) && q) {
            LONG i = InterlockedIncrement(&g_queueCount) - 1;
            if (i < 32) { g_queues[i].q = q; g_queues[i].type = d->Type; }
            q->Release();
        }
        HookQueue(*pp);
    }
    if (!IsInternal()) Log("ID3D12Device::CreateCommandQueue(type=%d prio=%d flags=0x%x) -> %s", d ? d->Type : -1, d ? d->Priority : 0, d ? d->Flags : 0, HrName(hr));
    return hr;
}
static void HookQueue(void* pq);
static HRESULT STDMETHODCALLTYPE H_CreateCommandQueue1(ID3D12Device* t, const D3D12_COMMAND_QUEUE_DESC* d, REFIID creator, REFIID r, void** pp) {
    HRESULT hr = o_CreateCommandQueue1(t, d, creator, r, pp);
    AddCrumb("ID3D12Device9::CreateCommandQueue1", hr);
    Log("ID3D12Device9::CreateCommandQueue1(type=%d prio=%d flags=0x%x) -> %s", d ? d->Type : -1, d ? d->Priority : 0, d ? d->Flags : 0, HrName(hr));
    if (SUCCEEDED(hr) && pp && *pp && d) {
        ID3D12CommandQueue* q = nullptr;
        if (SUCCEEDED(((IUnknown*)*pp)->QueryInterface(IID_ID3D12CommandQueue, (void**)&q)) && q) {
            LONG i = InterlockedIncrement(&g_queueCount) - 1;
            if (i < 32) { g_queues[i].q = q; g_queues[i].type = d->Type; }
            q->Release();
        }
        HookQueue(*pp);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateCommandAllocator(ID3D12Device* t, D3D12_COMMAND_LIST_TYPE ty, REFIID r, void** pp) {
    HRESULT hr = o_CreateCommandAllocator(t, ty, r, pp);
    AddCrumb("ID3D12Device::CreateCommandAllocator", hr);
    if (FAILED(hr) && FailBudget()) { InterlockedIncrement(&c_fail); Log("!! CreateCommandAllocator(type=%d) FAILED %s", ty, HrName(hr)); }
    return hr;
}

// =============================================================================
//  RecordLists: PSO registry + per-command-list call recorder (finds what hangs the GPU)
// =============================================================================
static bool PatchSlot(void** vt, int idx, void* hook, void** orig);
static bool SubobjectInner(int type, size_t* size, size_t* align);
struct PsoRec { void* pso; uint64_t h[3]; char desc[160]; };
static PsoRec g_psoRecs[4096]; static LONG g_psoRecCount;
static LONG g_skipped;
static void PsoAdd(void* pso, const char* kind, const D3D12_SHADER_BYTECODE* bc, int nbc) {
    if (!g_cfg.recordLists || !pso) return;
    LONG i = InterlockedIncrement(&g_psoRecCount) - 1;
    if (i >= 4096) return;
    PsoRec& r = g_psoRecs[i]; memset(&r, 0, sizeof r);
    size_t k = snprintf(r.desc, sizeof r.desc, "%s", kind);
    for (int j = 0; j < nbc && j < 3; j++) {
        if (!bc[j].pShaderBytecode || !bc[j].BytecodeLength || !Readable(bc[j].pShaderBytecode, bc[j].BytecodeLength)) continue;
        r.h[j] = Fnv1a(bc[j].pShaderBytecode, bc[j].BytecodeLength);
        static const char* sn[3] = { "cs/vs", "ps", "x" };
        char d[160]; DescribeShader(bc[j].pShaderBytecode, bc[j].BytecodeLength, d, sizeof d); d[40] = 0;
        if (k < sizeof r.desc) k += snprintf(r.desc + k, sizeof r.desc - k, " %s=%016llx(%s)", sn[j], (unsigned long long)r.h[j], d);
        DumpBlob("pso", kind, bc[j].pShaderBytecode, bc[j].BytecodeLength);
    }
    MemoryBarrier(); r.pso = pso;
}
static const PsoRec* PsoFind(void* pso) {
    LONG n = g_psoRecCount < 4096 ? g_psoRecCount : 4096;
    for (LONG i = n - 1; i >= 0; i--) if (g_psoRecs[i].pso == pso) return &g_psoRecs[i];   // newest first (addresses get reused)
    return nullptr;
}
static void PsoAddStream(void* pso, const D3D12_PIPELINE_STATE_STREAM_DESC* sd) {
    if (!g_cfg.recordLists || !pso || !sd || !sd->pPipelineStateSubobjectStream || !Readable(sd->pPipelineStateSubobjectStream, sd->SizeInBytes)) return;
    const uint8_t* b = (const uint8_t*)sd->pPipelineStateSubobjectStream; size_t off = 0;
    D3D12_SHADER_BYTECODE bc[3] = {}; int n = 0; const char* kind = "stream";
    while (off + 4 <= sd->SizeInBytes) {
        int type = *(const int*)(b + off); size_t sz, al;
        if (!SubobjectInner(type, &sz, &al)) break;
        size_t inner = (off + 4 + al - 1) & ~(al - 1);
        if (inner + sz > sd->SizeInBytes) break;
        const uint8_t* p = b + inner;
        if (type == 6) { bc[0] = *(const D3D12_SHADER_BYTECODE*)p; kind = "cs"; n = 1; }
        else if (type == 1 && n < 3) { bc[0] = *(const D3D12_SHADER_BYTECODE*)p; kind = "gfx"; if (n < 1) n = 1; }
        else if (type == 2) { bc[1] = *(const D3D12_SHADER_BYTECODE*)p; if (n < 2) n = 2; }
        off = (inner + sz + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
    }
    PsoAdd(pso, kind, bc, n);
}
struct ListOp { uint8_t type; void* pso; uint32_t a, b, c, d; };
struct ListRec { void* list; LONG n; LONG overflow; void* curPso; ListOp ops[3000]; };
static ListRec* g_lists[64]; static LONG g_listCount; static thread_local uint32_t g_opD;
static ListRec* ListGet(void* l, bool create) {
    for (LONG i = 0; i < g_listCount && i < 64; i++) if (g_lists[i] && g_lists[i]->list == l) return g_lists[i];
    if (!create) return nullptr;
    LONG i = InterlockedIncrement(&g_listCount) - 1; if (i >= 64) return nullptr;
    ListRec* r = (ListRec*)VirtualAlloc(nullptr, sizeof(ListRec), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!r) return nullptr;
    r->list = l; g_lists[i] = r; return r;
}
static bool PsoSkipped(void* pso) {
    if (!g_skipCount) return false;
    const PsoRec* r = PsoFind(pso); if (!r) return false;
    for (int j = 0; j < 3; j++) for (int k = 0; k < g_skipCount; k++) if (r->h[j] && r->h[j] == g_skipHash[k]) return true;
    return false;
}
static void ListPush(void* l, uint8_t type, uint32_t a, uint32_t b, uint32_t c) {
    ListRec* r = ListGet(l, true); if (!r) return;
    if (r->n < 3000) { r->ops[r->n++] = { type, r->curPso, a, b, c, g_opD }; g_opD = 0; } else r->overflow++;
}
typedef void (STDMETHODCALLTYPE *PFN_L_Dispatch)(void*, UINT, UINT, UINT);
typedef void (STDMETHODCALLTYPE *PFN_L_Draw)(void*, UINT, UINT, UINT, UINT);
typedef void (STDMETHODCALLTYPE *PFN_L_DrawIdx)(void*, UINT, UINT, UINT, INT, UINT);
typedef void (STDMETHODCALLTYPE *PFN_L_SetPSO)(void*, void*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_L_Reset)(void*, void*, void*);
typedef void (STDMETHODCALLTYPE *PFN_L_ExecInd)(void*, void*, UINT, void*, UINT64, void*, UINT64);
struct ListVt { void** vt; int type; void* ex[16]; PFN_L_Dispatch dispatch; PFN_L_Draw draw; PFN_L_DrawIdx drawIdx; PFN_L_SetPSO setPso; PFN_L_Reset reset; PFN_L_ExecInd execInd; };
static ListVt g_listVts[6]; static LONG g_listVtCount; static void** g_listVtbl;
static ListVt* VtFor(void* l) {
    void** vt = *(void***)l; LONG n = g_listVtCount < 6 ? g_listVtCount : 6;
    for (LONG i = 0; i < n; i++) if (g_listVts[i].vt == vt) return &g_listVts[i];
    // Unknown vtable (another module cloned a patched vtable): pick a hooked entry of the same list type.
    int ty = (int)((ID3D12CommandList*)l)->GetType(); ListVt* best = nullptr;
    for (LONG i = 0; i < n; i++) if (g_listVts[i].type == ty && g_listVts[i].dispatch) { best = &g_listVts[i]; break; }
    if (!best) for (LONG i = 0; i < n; i++) if (g_listVts[i].dispatch) { best = &g_listVts[i]; break; }
    static volatile LONG warned; if (InterlockedIncrement(&warned) <= 3) Log("[lists] list %p has an unknown vtable %p - using the originals of list type %d", l, (void*)vt, ty);
    return best;
}
static void STDMETHODCALLTYPE H_L_Dispatch(void* l, UINT x, UINT y, UINT z) {
    ListRec* r = ListGet(l, true);
    if (g_cfg.skipDispatches || (r && PsoSkipped(r->curPso))) { InterlockedIncrement(&g_skipped); ListPush(l, 'S', x, y, z); return; }
    ListPush(l, 'D', x, y, z); VtFor(l)->dispatch(l, x, y, z);
}
static void STDMETHODCALLTYPE H_L_Draw(void* l, UINT a, UINT b, UINT c, UINT d) {
    ListRec* r = ListGet(l, true);
    if (g_cfg.skipDraws || (r && PsoSkipped(r->curPso))) { InterlockedIncrement(&g_skipped); ListPush(l, 's', a, b, 0); return; }
    ListPush(l, 'W', a, b, 0); VtFor(l)->draw(l, a, b, c, d);
}
static void STDMETHODCALLTYPE H_L_DrawIdx(void* l, UINT a, UINT b, UINT c, INT d, UINT e) {
    ListRec* r = ListGet(l, true);
    if (g_cfg.skipDraws || (r && PsoSkipped(r->curPso))) { InterlockedIncrement(&g_skipped); ListPush(l, 's', a, b, 0); return; }
    ListPush(l, 'I', a, b, 0); VtFor(l)->drawIdx(l, a, b, c, d, e);
}
static void STDMETHODCALLTYPE H_L_ExecInd(void* l, void* sig, UINT max, void* args, UINT64 ao, void* cnt, UINT64 co) {
    ListRec* r = ListGet(l, true);
    if (r && PsoSkipped(r->curPso)) { InterlockedIncrement(&g_skipped); ListPush(l, 's', max, 0, 0); return; }
    ListPush(l, 'E', max, cnt ? 1 : 0, 0); VtFor(l)->execInd(l, sig, max, args, ao, cnt, co);
}
static void STDMETHODCALLTYPE H_L_SetPSO(void* l, void* pso) { ListRec* r = ListGet(l, true); if (r) r->curPso = pso; VtFor(l)->setPso(l, pso); }
static HRESULT STDMETHODCALLTYPE H_L_Reset(void* l, void* alloc, void* pso) {
    ListRec* r = ListGet(l, true); if (r) { r->n = 0; r->overflow = 0; r->curPso = pso; }
    return VtFor(l)->reset(l, alloc, pso);
}

// ---- more recorded ops: root arguments, barriers, copies, clears ----
static inline uint32_t Lo(const void* p) { return (uint32_t)(uintptr_t)p; }
enum { X_RC32 = 0, X_RC32S, X_RUAV, X_RSRV, X_RCBV, X_RTAB, X_BARRIER, X_COPYBUF, X_COPYRES, X_COPYTEX, X_CLRUINT, X_CLRFLT, X_CLRRTV, X_CLRDSV, X_OMRT };
// ---- RTV/DSV descriptor tracking: CPU handle -> resource (so a clear can be traced to the resource it hits) ----
struct DescRec { volatile size_t key; void* res; int fmt, dim, kind; };
#define DESC_SLOTS (1 << 16)
static DescRec* g_desc;
static DescRec* DescSlot(size_t k, bool insert) {
    if (!k) return nullptr;
    if (!g_desc) {
        DescRec* mem = (DescRec*)VirtualAlloc(nullptr, sizeof(DescRec) * DESC_SLOTS, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (InterlockedCompareExchangePointer((void* volatile*)&g_desc, mem, nullptr) != nullptr && mem) VirtualFree(mem, 0, MEM_RELEASE);
        if (!g_desc) return nullptr;
    }
    uint32_t i = (uint32_t)(((k >> 3) * 0x9E3779B97F4A7C15ull) >> (64 - 16));
    for (int n = 0; n < 64; n++, i = (i + 1) & (DESC_SLOTS - 1)) {
        size_t c = g_desc[i].key;
        if (c == k) return &g_desc[i];
        if (!c) {
            if (!insert) return nullptr;
            if ((size_t)InterlockedCompareExchangePointer((void* volatile*)&g_desc[i].key, (void*)k, nullptr) == 0) return &g_desc[i];
            if (g_desc[i].key == k) return &g_desc[i];
        }
    }
    return nullptr;
}
static void DescSet(size_t k, void* res, int fmt, int dim, int kind) {
    DescRec* d = DescSlot(k, true); if (!d) return;
    d->res = res; d->fmt = fmt; d->dim = dim; d->kind = kind;
}
static void STDMETHODCALLTYPE H_CreateRenderTargetView(ID3D12Device* t, ID3D12Resource* res, const D3D12_RENDER_TARGET_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    o_CreateRenderTargetView(t, res, d, h);
    int fmt = 0, dim = 0; if (d && Readable(d, sizeof *d)) { fmt = d->Format; dim = d->ViewDimension; }
    else { ResRec* r = ResSlot(res, false); if (r) fmt = r->fmt; }
    DescSet(h.ptr, res, fmt, dim, 0);
}
static void STDMETHODCALLTYPE H_CreateDepthStencilView(ID3D12Device* t, ID3D12Resource* res, const D3D12_DEPTH_STENCIL_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    o_CreateDepthStencilView(t, res, d, h);
    int fmt = 0, dim = 0; if (d && Readable(d, sizeof *d)) { fmt = d->Format; dim = d->ViewDimension; }
    else { ResRec* r = ResSlot(res, false); if (r) fmt = r->fmt; }
    DescSet(h.ptr, res, fmt, dim, 1);
}
static void DescCopy1(ID3D12Device* t, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src, UINT n, D3D12_DESCRIPTOR_HEAP_TYPE ty, size_t inc) {
    (void)t; (void)ty;
    for (UINT i = 0; i < n && i < 65536; i++) {
        DescRec* s = DescSlot(src.ptr + (size_t)i * inc, false);
        if (s && s->res) DescSet(dst.ptr + (size_t)i * inc, s->res, s->fmt, s->dim, s->kind);
        else { DescRec* d = DescSlot(dst.ptr + (size_t)i * inc, false); if (d) { d->res = nullptr; d->fmt = 0; } }
    }
}
static void STDMETHODCALLTYPE H_CopyDescriptorsSimple(ID3D12Device* t, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE ty) {
    o_CopyDescriptorsSimple(t, n, dst, src, ty);
    if (ty == D3D12_DESCRIPTOR_HEAP_TYPE_RTV || ty == D3D12_DESCRIPTOR_HEAP_TYPE_DSV) DescCopy1(t, dst, src, n, ty, t->GetDescriptorHandleIncrementSize(ty));
}
static void STDMETHODCALLTYPE H_CopyDescriptors(ID3D12Device* t, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* ds, const UINT* dsz, UINT ns, const D3D12_CPU_DESCRIPTOR_HANDLE* ss, const UINT* ssz, D3D12_DESCRIPTOR_HEAP_TYPE ty) {
    o_CopyDescriptors(t, nd, ds, dsz, ns, ss, ssz, ty);
    if ((ty != D3D12_DESCRIPTOR_HEAP_TYPE_RTV && ty != D3D12_DESCRIPTOR_HEAP_TYPE_DSV) || !ds || !ss || nd > 256 || ns > 256) return;
    size_t inc = t->GetDescriptorHandleIncrementSize(ty);
    UINT di = 0, doff = 0, si = 0, soff = 0;
    while (di < nd && si < ns) {
        UINT dsize = dsz ? dsz[di] : 1, ssize = ssz ? ssz[si] : 1;
        UINT dl = dsize - doff, sl = ssize - soff, n = dl < sl ? dl : sl;
        D3D12_CPU_DESCRIPTOR_HANDLE d = { ds[di].ptr + (size_t)doff * inc }, s = { ss[si].ptr + (size_t)soff * inc };
        DescCopy1(t, d, s, n, ty, inc);
        doff += n; soff += n;
        if (doff >= dsize) { di++; doff = 0; }
        if (soff >= ssize) { si++; soff = 0; }
    }
}
static void LogClear(void* l, const char* what, size_t h) {
    static volatile LONG nlog, nunk;
    DescRec* d = DescSlot(h, false);
    bool unk = !d || !d->res;
    if (unk ? (InterlockedIncrement(&nunk) > 60) : (InterlockedIncrement(&nlog) > 150)) return;
    if (unk) { Log("[rtv] %s list=%p handle=%p -> UNKNOWN descriptor (never created through CreateRenderTargetView/DepthStencilView or copied)", what, l, (void*)h); return; }
    ResRec* r = ResSlot(d->res, false);
    if (r) Log("[rtv] %s list=%p handle=%p -> res=%p %s %llux%u fmt=%s mips=%u flags=0x%x heap=%s created by %s t=%.2fs | view fmt=%s dim=%d",
               what, l, (void*)h, d->res, DimName(r->dim), (unsigned long long)r->width, r->height, FormatName(r->fmt), r->mips, r->flags, HeapTypeName(r->heapType), r->fn ? r->fn : "?", r->t, FormatName(d->fmt), d->dim);
    else Log("[rtv] %s list=%p handle=%p -> res=%p (resource not in registry) view fmt=%s dim=%d", what, l, (void*)h, d->res, FormatName(d->fmt), d->dim);
}
static void STDMETHODCALLTYPE H_L_ClrRTV(void* l, size_t h, const FLOAT c[4], UINT nr, const D3D12_RECT* r) {
    DescRec* d = DescSlot(h, false); LogClear(l, "ClearRenderTargetView", h);
    ListPush(l, 'k', (uint32_t)h, d ? Lo(d->res) : 0, nr); 
    if (g_cfg.skipClears) return;
    ((void (STDMETHODCALLTYPE*)(void*, size_t, const FLOAT*, UINT, const D3D12_RECT*))VtFor(l)->ex[X_CLRRTV])(l, h, c, nr, r);
}
static void STDMETHODCALLTYPE H_L_ClrDSV(void* l, size_t h, D3D12_CLEAR_FLAGS f, FLOAT dp, UINT8 st, UINT nr, const D3D12_RECT* r) {
    DescRec* d = DescSlot(h, false); LogClear(l, "ClearDepthStencilView", h);
    ListPush(l, 'K', (uint32_t)h, d ? Lo(d->res) : 0, nr);
    if (g_cfg.skipClears) return;
    ((void (STDMETHODCALLTYPE*)(void*, size_t, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*))VtFor(l)->ex[X_CLRDSV])(l, h, f, dp, st, nr, r);
}
static void STDMETHODCALLTYPE H_L_OMRT(void* l, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rt, BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* ds) {
    size_t h0 = (rt && n && Readable(rt, 8)) ? rt[0].ptr : 0;
    DescRec* d = DescSlot(h0, false);
    ListPush(l, 'O', n, (uint32_t)h0, d ? Lo(d->res) : 0);
    ((void (STDMETHODCALLTYPE*)(void*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*))VtFor(l)->ex[X_OMRT])(l, n, rt, single, ds);
}
static void STDMETHODCALLTYPE H_L_RC32S(void* l, UINT idx, UINT n, const UINT* d, UINT off) {
    g_opD = (d && n > 1 && Readable(d, 8)) ? d[1] : 0; ListPush(l, 'R', idx, n | (off << 16), (d && Readable(d, 4)) ? d[0] : 0);
    ((void (STDMETHODCALLTYPE*)(void*, UINT, UINT, const UINT*, UINT))VtFor(l)->ex[X_RC32S])(l, idx, n, d, off);
}
static void STDMETHODCALLTYPE H_L_RC32(void* l, UINT idx, UINT v, UINT off) {
    ListPush(l, 'r', idx, off, v); ((void (STDMETHODCALLTYPE*)(void*, UINT, UINT, UINT))VtFor(l)->ex[X_RC32])(l, idx, v, off);
}
static void STDMETHODCALLTYPE H_L_RUAV(void* l, UINT idx, UINT64 va) {
    ListPush(l, 'U', idx, (uint32_t)va, (uint32_t)(va >> 32)); ((void (STDMETHODCALLTYPE*)(void*, UINT, UINT64))VtFor(l)->ex[X_RUAV])(l, idx, va);
}
static void STDMETHODCALLTYPE H_L_RSRV(void* l, UINT idx, UINT64 va) {
    ListPush(l, 'V', idx, (uint32_t)va, (uint32_t)(va >> 32)); ((void (STDMETHODCALLTYPE*)(void*, UINT, UINT64))VtFor(l)->ex[X_RSRV])(l, idx, va);
}
static void STDMETHODCALLTYPE H_L_RCBV(void* l, UINT idx, UINT64 va) {
    ListPush(l, 'C', idx, (uint32_t)va, (uint32_t)(va >> 32)); ((void (STDMETHODCALLTYPE*)(void*, UINT, UINT64))VtFor(l)->ex[X_RCBV])(l, idx, va);
}
static void STDMETHODCALLTYPE H_L_RTAB(void* l, UINT idx, UINT64 h) {
    ListPush(l, 'T', idx, (uint32_t)h, (uint32_t)(h >> 32)); ((void (STDMETHODCALLTYPE*)(void*, UINT, UINT64))VtFor(l)->ex[X_RTAB])(l, idx, h);
}
static void STDMETHODCALLTYPE H_L_Barrier(void* l, UINT n, const D3D12_RESOURCE_BARRIER* b) {
    uint32_t types = 0; uint32_t r0 = 0, st = 0;
    for (UINT i = 0; b && i < n && i < 64 && Readable(b + i, sizeof *b); i++) {
        types |= 1u << (int)b[i].Type;
        if (i == 0) {
            if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) { r0 = Lo(b[i].Transition.pResource); st = (b[i].Transition.StateBefore << 16) | (b[i].Transition.StateAfter & 0xffff); }
            else if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) r0 = Lo(b[i].Aliasing.pResourceAfter);
            else r0 = Lo(b[i].UAV.pResource);
        }
    }
    g_opD = st; ListPush(l, 'B', n, types, r0);
    ((void (STDMETHODCALLTYPE*)(void*, UINT, const D3D12_RESOURCE_BARRIER*))VtFor(l)->ex[X_BARRIER])(l, n, b);
}
static void STDMETHODCALLTYPE H_L_CopyBuf(void* l, void* dst, UINT64 dof, void* src, UINT64 sof, UINT64 n) {
    ListPush(l, 'c', Lo(dst), Lo(src), (uint32_t)n); ((void (STDMETHODCALLTYPE*)(void*, void*, UINT64, void*, UINT64, UINT64))VtFor(l)->ex[X_COPYBUF])(l, dst, dof, src, sof, n);
}
static void STDMETHODCALLTYPE H_L_CopyRes(void* l, void* dst, void* src) {
    ListPush(l, 'y', Lo(dst), Lo(src), 0); ((void (STDMETHODCALLTYPE*)(void*, void*, void*))VtFor(l)->ex[X_COPYRES])(l, dst, src);
}
static void STDMETHODCALLTYPE H_L_CopyTex(void* l, const D3D12_TEXTURE_COPY_LOCATION* d, UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* s, const D3D12_BOX* b) {
    ListPush(l, 't', d ? Lo(d->pResource) : 0, s ? Lo(s->pResource) : 0, 0);
    ((void (STDMETHODCALLTYPE*)(void*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*))VtFor(l)->ex[X_COPYTEX])(l, d, x, y, z, s, b);
}
static void STDMETHODCALLTYPE H_L_ClrUint(void* l, UINT64 gh, size_t ch, void* res, const UINT v[4], UINT nr, const D3D12_RECT* r) {
    ListPush(l, 'u', Lo(res), v ? v[0] : 0, nr); ((void (STDMETHODCALLTYPE*)(void*, UINT64, size_t, void*, const UINT*, UINT, const D3D12_RECT*))VtFor(l)->ex[X_CLRUINT])(l, gh, ch, res, v, nr, r);
}
static void STDMETHODCALLTYPE H_L_ClrFlt(void* l, UINT64 gh, size_t ch, void* res, const FLOAT v[4], UINT nr, const D3D12_RECT* r) {
    ListPush(l, 'f', Lo(res), v ? *(const uint32_t*)v : 0, nr); ((void (STDMETHODCALLTYPE*)(void*, UINT64, size_t, void*, const FLOAT*, UINT, const D3D12_RECT*))VtFor(l)->ex[X_CLRFLT])(l, gh, ch, res, v, nr, r);
}
static void HookListVtbl(ID3D12CommandList* l) {
    if (!g_cfg.recordLists || !l) return;
    void** vt = *(void***)l;
    { void** vt0 = *(void***)l; LONG n0 = g_listVtCount < 6 ? g_listVtCount : 6; for (LONG i = 0; i < n0; i++) if (g_listVts[i].vt == vt0) return; }
    LONG k = InterlockedIncrement(&g_listVtCount) - 1;
    if (k >= 6) return;
    ListVt& v = g_listVts[k]; v.type = (int)l->GetType(); g_listVtbl = vt; MemoryBarrier(); v.vt = vt;
    PatchSlot(vt, IDX_List_Reset, (void*)&H_L_Reset, (void**)&v.reset);
    PatchSlot(vt, IDX_List_DrawInstanced, (void*)&H_L_Draw, (void**)&v.draw);
    PatchSlot(vt, IDX_List_DrawIndexedInstanced, (void*)&H_L_DrawIdx, (void**)&v.drawIdx);
    PatchSlot(vt, IDX_List_Dispatch, (void*)&H_L_Dispatch, (void**)&v.dispatch);
    PatchSlot(vt, IDX_List_SetPipelineState, (void*)&H_L_SetPSO, (void**)&v.setPso);
    PatchSlot(vt, IDX_List_ExecuteIndirect, (void*)&H_L_ExecInd, (void**)&v.execInd);
    if (l->GetType() != D3D12_COMMAND_LIST_TYPE_COPY) {
        PatchSlot(vt, 35, (void*)&H_L_RC32S, &v.ex[X_RC32S]);  PatchSlot(vt, 33, (void*)&H_L_RC32, &v.ex[X_RC32]);
        PatchSlot(vt, 41, (void*)&H_L_RUAV, &v.ex[X_RUAV]);    PatchSlot(vt, 39, (void*)&H_L_RSRV, &v.ex[X_RSRV]);
        PatchSlot(vt, 37, (void*)&H_L_RCBV, &v.ex[X_RCBV]);    PatchSlot(vt, 31, (void*)&H_L_RTAB, &v.ex[X_RTAB]);
        PatchSlot(vt, 49, (void*)&H_L_ClrUint, &v.ex[X_CLRUINT]); PatchSlot(vt, 50, (void*)&H_L_ClrFlt, &v.ex[X_CLRFLT]);
        PatchSlot(vt, IDX_List_ClearRenderTargetView, (void*)&H_L_ClrRTV, &v.ex[X_CLRRTV]); PatchSlot(vt, IDX_List_ClearDepthStencilView, (void*)&H_L_ClrDSV, &v.ex[X_CLRDSV]);
        PatchSlot(vt, IDX_List_OMSetRenderTargets, (void*)&H_L_OMRT, &v.ex[X_OMRT]);
    }
    PatchSlot(vt, 26, (void*)&H_L_Barrier, &v.ex[X_BARRIER]);
    PatchSlot(vt, 15, (void*)&H_L_CopyBuf, &v.ex[X_COPYBUF]); PatchSlot(vt, 17, (void*)&H_L_CopyRes, &v.ex[X_COPYRES]); PatchSlot(vt, 16, (void*)&H_L_CopyTex, &v.ex[X_COPYTEX]);
    // A vtable cloned from an already-patched one holds our hooks without us seeing the originals: borrow them from a sibling of the same type.
    for (LONG i = 0; i < k; i++) if (g_listVts[i].type == v.type && g_listVts[i].dispatch) {
        ListVt& o = g_listVts[i];
        if (!v.dispatch) v.dispatch = o.dispatch; if (!v.draw) v.draw = o.draw; if (!v.drawIdx) v.drawIdx = o.drawIdx;
        if (!v.setPso) v.setPso = o.setPso; if (!v.reset) v.reset = o.reset; if (!v.execInd) v.execInd = o.execInd;
        for (int j = 0; j < 16; j++) if (!v.ex[j]) v.ex[j] = o.ex[j];
        break;
    }
    Log("[lists] command list vtable %p hooked (type %d)", (void*)vt, (int)l->GetType());
}
static void DumpListOps(void* l, const char* why) {
    ListRec* r = ListGet(l, false);
    if (!r) { Log("[lists] %s: list %p has no recording (recorded before hooks were installed)", why, l); return; }
    Log("[lists] %s: list %p recorded %ld ops%s", why, l, r->n, r->overflow ? " (more than 3000, truncated)" : "");
    void* last = (void*)-1; int shown = 0;
    for (LONG i = 0; i < r->n && shown < 400; i++) {
        const ListOp& o = r->ops[i];
        const PsoRec* p = PsoFind(o.pso);
        if (o.type == 'D' || o.type == 'W' || o.type == 'I' || o.type == 'S' || o.type == 's' || o.type == 'E') Log("   [%ld] %c(%u,%u,%u) pso=%p %s", i, o.type, o.a, o.b, o.c, o.pso, p ? p->desc : "(unknown pso)");
        else Log("   [%ld] %c a=%08x b=%08x c=%08x d=%08x", i, o.type, o.a, o.b, o.c, o.d);
        last = o.pso; shown++;
    }
    (void)last;
}

static void HookListVtbl(ID3D12CommandList* l);
static HRESULT STDMETHODCALLTYPE H_CreateCommandList1(ID3D12Device* t, UINT nm, D3D12_COMMAND_LIST_TYPE ty, D3D12_COMMAND_LIST_FLAGS fl, REFIID r, void** pp) {
    HRESULT hr = o_CreateCommandList1(t, nm, ty, fl, r, pp);
    AddCrumb("ID3D12Device4::CreateCommandList1", hr);
    if (SUCCEEDED(hr) && pp && *pp && g_cfg.recordLists) { ID3D12CommandList* cl = nullptr; if (SUCCEEDED(((IUnknown*)*pp)->QueryInterface(IID_ID3D12CommandList, (void**)&cl)) && cl) { HookListVtbl(cl); cl->Release(); } }
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateGraphicsPipelineState(ID3D12Device* t, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, REFIID r, void** pp) {
    HRESULT hr = o_CreateGraphicsPipelineState(t, d, r, pp);
    InterlockedIncrement(&c_gpso);
    AddCrumb("ID3D12Device::CreateGraphicsPipelineState", hr);
    if (SUCCEEDED(hr) && pp && *pp && d) { D3D12_SHADER_BYTECODE b[2] = { d->VS, d->PS }; PsoAdd(*pp, "gfx", b, 2); }
    if (FAILED(hr) && d) {
        InterlockedIncrement(&c_fail);
        if (FailBudget()) {
            Log("!! CreateGraphicsPipelineState FAILED %s  RTs=%u [%s,%s] DSV=%s samples=%u topology=%d inputElems=%u flags=0x%x",
                HrName(hr), d->NumRenderTargets, FormatName(d->RTVFormats[0]), FormatName(d->RTVFormats[1]), FormatName(d->DSVFormat),
                d->SampleDesc.Count, d->PrimitiveTopologyType, d->InputLayout.NumElements, d->Flags);
            LogShader("gpso", "vs", d->VS); LogShader("gpso", "hs", d->HS); LogShader("gpso", "ds", d->DS);
            LogShader("gpso", "gs", d->GS); LogShader("gpso", "ps", d->PS);
        }
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateComputePipelineState(ID3D12Device* t, const D3D12_COMPUTE_PIPELINE_STATE_DESC* d, REFIID r, void** pp) {
    HRESULT hr = o_CreateComputePipelineState(t, d, r, pp);
    InterlockedIncrement(&c_cpso);
    AddCrumb("ID3D12Device::CreateComputePipelineState", hr);
    if (SUCCEEDED(hr) && pp && *pp && d) { D3D12_SHADER_BYTECODE b[1] = { d->CS }; PsoAdd(*pp, "cs", b, 1); }
    if (FAILED(hr) && d) {
        InterlockedIncrement(&c_fail);
        if (FailBudget()) { Log("!! CreateComputePipelineState FAILED %s flags=0x%x", HrName(hr), d->Flags); LogShader("cpso", "cs", d->CS); }
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateCommandList(ID3D12Device* t, UINT nm, D3D12_COMMAND_LIST_TYPE ty, ID3D12CommandAllocator* a, ID3D12PipelineState* ps, REFIID r, void** pp) {
    HRESULT hr = o_CreateCommandList(t, nm, ty, a, ps, r, pp);
    AddCrumb("ID3D12Device::CreateCommandList", hr);
    if (FAILED(hr) && FailBudget()) { InterlockedIncrement(&c_fail); Log("!! CreateCommandList(type=%d) FAILED %s", ty, HrName(hr)); }
    if (SUCCEEDED(hr) && pp && *pp && g_cfg.recordLists) { ID3D12CommandList* cl = nullptr; if (SUCCEEDED(((IUnknown*)*pp)->QueryInterface(IID_ID3D12CommandList, (void**)&cl)) && cl) { HookListVtbl(cl); cl->Release(); } }
    return hr;
}
// Overwrite the answer the game gets with the [Spoof] values
static volatile LONG g_spoofLogged[128];
static void ApplySpoof(D3D12_FEATURE f, void* d, UINT sz, int inSM) {
    const Spoof& s = g_spoof;
    char what[300]; what[0] = 0; size_t k = 0;
#define SET(field, v) if ((v) != -1 && (int)(field) != (v)) { k += snprintf(what + k, sizeof what - k, " " #field " %d->%d", (int)(field), (v)); field = (decltype(field))(v); }
#define SETB(field, v) if ((v) != -1 && (int)(field) != (v)) { k += snprintf(what + k, sizeof what - k, " " #field " %d->%d", (int)(field), (v)); field = (v) ? TRUE : FALSE; }
    switch ((int)f) {
    case D3D12_FEATURE_FEATURE_LEVELS: if (sz >= sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS) && s.maxFeatureLevel != -1) {
        auto* x = (D3D12_FEATURE_DATA_FEATURE_LEVELS*)d;
        int best = 0;
        for (UINT i = 0; x->pFeatureLevelsRequested && i < x->NumFeatureLevels; i++)
            if ((int)x->pFeatureLevelsRequested[i] <= s.maxFeatureLevel && (int)x->pFeatureLevelsRequested[i] > best) best = x->pFeatureLevelsRequested[i];
        if (best && best != (int)x->MaxSupportedFeatureLevel) { k += snprintf(what, sizeof what, " MaxSupportedFeatureLevel %s->%s", FlName(x->MaxSupportedFeatureLevel), FlName((D3D_FEATURE_LEVEL)best)); x->MaxSupportedFeatureLevel = (D3D_FEATURE_LEVEL)best; }
    } break;
    case D3D12_FEATURE_SHADER_MODEL: if (sz >= sizeof(D3D12_FEATURE_DATA_SHADER_MODEL) && s.highestShaderModel != -1) {
        auto* x = (D3D12_FEATURE_DATA_SHADER_MODEL*)d;
        int v = s.highestShaderModel < inSM ? s.highestShaderModel : inSM;
        if (v != (int)x->HighestShaderModel) { k += snprintf(what, sizeof what, " HighestShaderModel %x->%x", (int)x->HighestShaderModel, v); x->HighestShaderModel = (D3D_SHADER_MODEL)v; }
    } break;
    case D3D12_FEATURE_D3D12_OPTIONS: if (sz >= sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS)) { auto* x = (D3D12_FEATURE_DATA_D3D12_OPTIONS*)d;
        SET(x->ResourceBindingTier, s.resourceBindingTier) SET(x->TiledResourcesTier, s.tiledResourcesTier) SET(x->ResourceHeapTier, s.resourceHeapTier)
        SET(x->ConservativeRasterizationTier, s.conservativeRasterTier) SETB(x->ROVsSupported, s.rovs) SETB(x->TypedUAVLoadAdditionalFormats, s.typedUAVLoadAdditionalFormats)
        SETB(x->PSSpecifiedStencilRefSupported, s.psStencilRef) SETB(x->VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation, s.vpRtArrayIndex) } break;
    case D3D12_FEATURE_D3D12_OPTIONS1: if (sz >= sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS1)) { auto* x = (D3D12_FEATURE_DATA_D3D12_OPTIONS1*)d;
        SETB(x->WaveOps, s.waveOps) SETB(x->Int64ShaderOps, s.int64ShaderOps) } break;
    case D3D12_FEATURE_D3D12_OPTIONS3: if (sz >= sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS3)) { auto* x = (D3D12_FEATURE_DATA_D3D12_OPTIONS3*)d;
        SETB(x->BarycentricsSupported, s.barycentrics) } break;
    case D3D12_FEATURE_D3D12_OPTIONS4: if (sz >= sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS4)) { auto* x = (D3D12_FEATURE_DATA_D3D12_OPTIONS4*)d;
        SETB(x->Native16BitShaderOpsSupported, s.native16Bit) } break;
    case D3D12_FEATURE_D3D12_OPTIONS5: if (sz >= sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5)) { auto* x = (D3D12_FEATURE_DATA_D3D12_OPTIONS5*)d;
        SET(x->RaytracingTier, s.raytracingTier) SET(x->RenderPassesTier, s.renderPassesTier) } break;
    case D3D12_FEATURE_D3D12_OPTIONS6: if (sz >= sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS6)) { auto* x = (D3D12_FEATURE_DATA_D3D12_OPTIONS6*)d;
        SET(x->VariableShadingRateTier, s.vrsTier) } break;
    case D3D12_FEATURE_D3D12_OPTIONS7: if (sz >= sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS7)) { auto* x = (D3D12_FEATURE_DATA_D3D12_OPTIONS7*)d;
        SET(x->MeshShaderTier, s.meshShaderTier) SET(x->SamplerFeedbackTier, s.samplerFeedbackTier) } break;
    case D3D12_FEATURE_D3D12_OPTIONS12: if (sz >= sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS12)) { auto* x = (D3D12_FEATURE_DATA_D3D12_OPTIONS12*)d;
        SETB(x->EnhancedBarriersSupported, s.enhancedBarriers) } break;
    case D3D12_FEATURE_D3D12_OPTIONS16: if (sz >= sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS16)) { auto* x = (D3D12_FEATURE_DATA_D3D12_OPTIONS16*)d;
        SETB(x->GPUUploadHeapSupported, s.gpuUploadHeap) } break;
    case D3D12_FEATURE_D3D12_TIGHT_ALIGNMENT: if (sz >= sizeof(D3D12_FEATURE_DATA_TIGHT_ALIGNMENT)) { auto* x = (D3D12_FEATURE_DATA_TIGHT_ALIGNMENT*)d;
        SET(x->SupportTier, s.tightAlignmentTier) } break;
    }
#undef SET
#undef SETB
    if (k && (int)f >= 0 && (int)f < 128 && InterlockedExchange(&g_spoofLogged[(int)f], 1) == 0)
        Log("[spoof] %s:%s", FeatureName(f), what);
}

static HRESULT STDMETHODCALLTYPE H_CheckFeatureSupport(ID3D12Device* t, D3D12_FEATURE f, void* d, UINT sz) {
    // remember inputs that are overwritten by the call
    int inSM = 0, inRS = 0;
    if (d && f == D3D12_FEATURE_SHADER_MODEL && sz >= sizeof(D3D12_FEATURE_DATA_SHADER_MODEL)) inSM = ((D3D12_FEATURE_DATA_SHADER_MODEL*)d)->HighestShaderModel;
    if (d && f == D3D12_FEATURE_ROOT_SIGNATURE && sz >= sizeof(D3D12_FEATURE_DATA_ROOT_SIGNATURE)) inRS = ((D3D12_FEATURE_DATA_ROOT_SIGNATURE*)d)->HighestVersion;
    HRESULT hr = o_CheckFeatureSupport(t, f, d, sz);
    InterlockedIncrement(&c_featureChecks);
    if (IsInternal()) return hr;
    if (SUCCEEDED(hr) && d && g_spoof.any) ApplySpoof(f, d, sz, inSM);
    if (!g_cfg.logFeatureChecks) return hr;
    uint64_t key = ((uint64_t)f << 40) ^ ((uint64_t)(uint32_t)hr << 8);
    if (d && (f == D3D12_FEATURE_FORMAT_SUPPORT || f == D3D12_FEATURE_FORMAT_INFO) && sz >= 4) key ^= (uint64_t)*(const DXGI_FORMAT*)d << 16;
    if (d && f == D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS && sz >= 8) key ^= ((uint64_t)*(const DXGI_FORMAT*)d << 16) ^ ((uint64_t)((const UINT*)d)[1] << 32);
    if (f == D3D12_FEATURE_SHADER_MODEL) key ^= (uint64_t)inSM << 16;
    if (f == D3D12_FEATURE_ROOT_SIGNATURE) key ^= (uint64_t)inRS << 16;
    if (FeatureSeen(key)) return hr;
    if (InterlockedIncrement(&g_featureLines) > g_cfg.maxFeatureLines) return hr;
    char desc[700] = "";
    if (SUCCEEDED(hr) && d) DescribeFeature(f, d, sz, desc, sizeof desc);
    char in[64] = "";
    if (f == D3D12_FEATURE_SHADER_MODEL) snprintf(in, sizeof in, " (asked up to %x.%x)", (inSM >> 4) & 0xF, inSM & 0xF);
    if (f == D3D12_FEATURE_ROOT_SIGNATURE) snprintf(in, sizeof in, " (asked up to 0x%x)", inRS);
    Log("CheckFeatureSupport(%s%s) -> %s %s", FeatureName(f), in, HrName(hr), desc);
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateDescriptorHeap(ID3D12Device* t, const D3D12_DESCRIPTOR_HEAP_DESC* d, REFIID r, void** pp) {
    HRESULT hr = o_CreateDescriptorHeap(t, d, r, pp);
    InterlockedIncrement(&c_descheap);
    AddCrumb("ID3D12Device::CreateDescriptorHeap", hr);
    if (!IsInternal() && d) {
        static const char* types[] = { "CBV_SRV_UAV", "SAMPLER", "RTV", "DSV" };
        if (FAILED(hr)) { InterlockedIncrement(&c_fail); if (FailBudget()) Log("!! CreateDescriptorHeap(%s, %u descriptors, flags=0x%x) FAILED %s", d->Type < 4 ? types[d->Type] : "?", d->NumDescriptors, d->Flags, HrName(hr)); }
        else if ((d->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) && FailBudget())
            Log("CreateDescriptorHeap(%s, %u descriptors, shader-visible) -> %s", d->Type < 4 ? types[d->Type] : "?", d->NumDescriptors, HrName(hr));
    }
    return hr;
}

static void DescribeRootSignatureBlob(const void* blob, SIZE_T len) {
    if (!r_CreateVersionedRootSignatureDeserializer || !blob || !len) return;
    ID3D12VersionedRootSignatureDeserializer* des = nullptr;
    InternalScope is;
    if (FAILED(r_CreateVersionedRootSignatureDeserializer(blob, len, IID_ID3D12VersionedRootSignatureDeserializer, (void**)&des)) || !des) { Log("    (root signature blob could not be deserialized)"); return; }
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* vd = nullptr;
    if (SUCCEEDED(des->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &vd)) && vd) {
        const D3D12_ROOT_SIGNATURE_DESC1& d = vd->Desc_1_1;
        Log("    root signature: %u params, %u static samplers, flags=0x%x", d.NumParameters, d.NumStaticSamplers, d.Flags);
        static const char* rt[] = { "SRV", "UAV", "CBV", "SAMPLER" };
        for (UINT i = 0; i < d.NumParameters && i < 64; i++) {
            const D3D12_ROOT_PARAMETER1& p = d.pParameters[i];
            if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
                char line[700]; size_t k = snprintf(line, sizeof line, "      [%u] table vis=%d:", i, p.ShaderVisibility);
                for (UINT j = 0; j < p.DescriptorTable.NumDescriptorRanges && j < 16 && k < sizeof line; j++) {
                    const D3D12_DESCRIPTOR_RANGE1& r = p.DescriptorTable.pDescriptorRanges[j];
                    char cnt[16]; if (r.NumDescriptors == UINT_MAX) strcpy(cnt, "unbounded"); else snprintf(cnt, sizeof cnt, "%u", r.NumDescriptors);
                    k += snprintf(line + k, sizeof line - k, " %s x%s @reg%u space%u flags=0x%x;", r.RangeType < 4 ? rt[r.RangeType] : "?", cnt, r.BaseShaderRegister, r.RegisterSpace, r.Flags);
                }
                Log("%s", line);
            } else if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
                Log("      [%u] %u root constants @b%u space%u", i, p.Constants.Num32BitValues, p.Constants.ShaderRegister, p.Constants.RegisterSpace);
            } else {
                static const char* dt[] = { "", "", "CBV", "SRV", "UAV" };
                Log("      [%u] root %s @reg%u space%u", i, p.ParameterType <= 4 ? dt[p.ParameterType] : "?", p.Descriptor.ShaderRegister, p.Descriptor.RegisterSpace);
            }
        }
    }
    des->Release();
}

static HRESULT STDMETHODCALLTYPE H_CreateRootSignature(ID3D12Device* t, UINT nm, const void* blob, SIZE_T len, REFIID r, void** pp) {
    HRESULT hr = o_CreateRootSignature(t, nm, blob, len, r, pp);
    InterlockedIncrement(&c_rootsig);
    AddCrumb("ID3D12Device::CreateRootSignature", hr);
    if (FAILED(hr) && !IsInternal()) {
        InterlockedIncrement(&c_fail);
        if (FailBudget()) { Log("!! CreateRootSignature(%zu bytes) FAILED %s", (size_t)len, HrName(hr)); DescribeRootSignatureBlob(blob, len); DumpBlob("rootsig", "rs", blob, len); }
    }
    return hr;
}
static volatile LONG64 g_substBytes; static volatile LONG g_substN;
static void VramLog(ID3D12Device* d, const char* why) {
    static IDXGIAdapter3* ad; static volatile LONG busy;
    if (!d || InterlockedExchange(&busy, 1)) return;
    InternalScope is;
    if (!ad) {
        typedef HRESULT (WINAPI *PFN_CF1)(REFIID, void**);
        HMODULE m = GetModuleHandleW(L"dxgi.dll"); PFN_CF1 cf = m ? (PFN_CF1)GetProcAddress(m, "CreateDXGIFactory1") : nullptr;
        IDXGIFactory4* f = nullptr;
        if (cf && SUCCEEDED(cf(__uuidof(IDXGIFactory4), (void**)&f)) && f) {
            IDXGIAdapter3* a = nullptr; LUID l; d->GetAdapterLuid(&l);
            if (SUCCEEDED(f->EnumAdapterByLuid(l, __uuidof(IDXGIAdapter3), (void**)&a))) ad = a;
            f->Release();
        }
    }
    if (ad) {
        DXGI_QUERY_VIDEO_MEMORY_INFO a = {}, b = {};
        ad->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &a);
        ad->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &b);
        Log("[vram] %s: local(VRAM) usage=%llu MB budget=%llu MB | non-local(shared) usage=%llu MB budget=%llu MB | tiled->committed swaps: %ld resources, %llu MB",
            why, (unsigned long long)(a.CurrentUsage >> 20), (unsigned long long)(a.Budget >> 20), (unsigned long long)(b.CurrentUsage >> 20), (unsigned long long)(b.Budget >> 20),
            g_substN, (unsigned long long)(g_substBytes >> 20));
    }
    { MEMORYSTATUSEX ms = { sizeof ms }; if (GlobalMemoryStatusEx(&ms))
        Log("[vram]   system: RAM free %llu / %llu MB | commit (RAM+pagefile) free %llu / %llu MB | process address space free %llu MB",
            (unsigned long long)(ms.ullAvailPhys >> 20), (unsigned long long)(ms.ullTotalPhys >> 20),
            (unsigned long long)(ms.ullAvailPageFile >> 20), (unsigned long long)(ms.ullTotalPageFile >> 20), (unsigned long long)(ms.ullAvailVirtual >> 20)); }
    InterlockedExchange(&busy, 0);
}
static HRESULT STDMETHODCALLTYPE H_CreateCommittedResource(ID3D12Device* t, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* cv, REFIID r, void** pp) {
    HRESULT hr = o_CreateCommittedResource(t, hp, hf, d, s, cv, r, pp);
    if (hr == E_OUTOFMEMORY && hp && hp->Type == D3D12_HEAP_TYPE_DEFAULT && !IsInternal()) {
        VramLog(t, "CreateCommittedResource E_OUTOFMEMORY");
        for (int i = 0; i < g_cfg.oomRetries && hr == E_OUTOFMEMORY; i++) {      // give the game's streamer a moment to free memory
            Sleep(100);
            hr = o_CreateCommittedResource(t, hp, hf, d, s, cv, r, pp);
            if (SUCCEEDED(hr)) Log("[oom] CreateCommittedResource succeeded on retry %d (%llu MB)", i + 1, d ? (unsigned long long)(d->Width >> 20) : 0ull);
        }
        if (hr == E_OUTOFMEMORY && g_cfg.oomRetries) Log("[oom] CreateCommittedResource still E_OUTOFMEMORY after %d retries", g_cfg.oomRetries);
        // Spill: put the buffer in system memory (L0, GPU-visible over PCIe, backed by RAM/pagefile) instead of failing.
        if (hr == E_OUTOFMEMORY && g_cfg.oomSysMem && d && d->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            D3D12_HEAP_PROPERTIES cp = {}; cp.Type = D3D12_HEAP_TYPE_CUSTOM; cp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK; cp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
            D3D12_RESOURCE_DESC cd = *d;
            HRESULT h2 = o_CreateCommittedResource(t, &cp, hf, &cd, s, cv, r, pp);
            Log("[oom] spill to system memory (custom heap L0 write-back): %llu MB buffer -> %s", (unsigned long long)(d->Width >> 20), HrName(h2));
            if (FAILED(h2) && (hf & D3D12_HEAP_FLAG_CREATE_NOT_ZEROED)) {
                h2 = o_CreateCommittedResource(t, &cp, (D3D12_HEAP_FLAGS)(hf & ~D3D12_HEAP_FLAG_CREATE_NOT_ZEROED), &cd, s, cv, r, pp);
                Log("[oom] spill retry without NOT_ZEROED -> %s", HrName(h2));
            }
            if (SUCCEEDED(h2)) hr = h2;
        }
    }
    InterlockedIncrement(&c_res);
    AddCrumb("ID3D12Device::CreateCommittedResource", hr);
    TrackRes("CreateCommittedResource", hr, hp, hf, d, pp);
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateHeap(ID3D12Device* t, const D3D12_HEAP_DESC* d, REFIID r, void** pp) {
    HRESULT hr = o_CreateHeap(t, d, r, pp);
    InterlockedIncrement(&c_heap);
    if (SUCCEEDED(hr)) RecordHeap(pp, d);
    AddCrumb("ID3D12Device::CreateHeap", hr);
    if (FAILED(hr) && FailBudget()) { InterlockedIncrement(&c_fail); Log("!! CreateHeap(%llu bytes, %s, flags=0x%x) FAILED %s", d ? (unsigned long long)d->SizeInBytes : 0ull, d ? HeapTypeName(d->Properties.Type) : "-", d ? d->Flags : 0, HrName(hr)); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreatePlacedResource(ID3D12Device* t, ID3D12Heap* h, UINT64 off, const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* cv, REFIID r, void** pp) {
    HRESULT hr = o_CreatePlacedResource(t, h, off, d, s, cv, r, pp);
    InterlockedIncrement(&c_res);
    AddCrumb("ID3D12Device::CreatePlacedResource", hr);
    { int hf2 = 0; const D3D12_HEAP_PROPERTIES* hp2 = PlacedHeapProps(h, &hf2); TrackRes("CreatePlacedResource", hr, hp2, hf2, d, pp); }
    return hr;
}
// ---- TileBacking: reserved (tiled) resources the game never maps get real memory ----
// On hardware with tiled tier 1 (GTX 960M) touching an unmapped tile is undefined and the driver
// removes the device. Pragmata creates a 1920x1080 tiled render target and relies on its relocation
// pass to map it; that pass never runs here. So: heap + UpdateTileMappings on the first direct queue
// submit after creation, unless the game maps the resource itself first.
struct PendingTiled { ID3D12Device* dev; ID3D12Resource* res; D3D12_RESOURCE_DESC desc; bool skip; };
static PendingTiled g_pendTiled[64]; static LONG g_pendTiledCount;
static CRITICAL_SECTION g_tileCs; static bool g_tileCsInit; static LONG c_tileBacked;
static void TileCsInit() { if (!g_tileCsInit) { InitializeCriticalSection(&g_tileCs); g_tileCsInit = true; } }
static void* g_substRes[256]; static LONG g_substCount;
static bool IsSubst(void* r) { LONG n = g_substCount < 256 ? g_substCount : 256; for (LONG i = 0; i < n; i++) if (g_substRes[i] == r) return true; return false; }
static HRESULT STDMETHODCALLTYPE H_CreateReservedResource(ID3D12Device* t, const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* cv, REFIID r, void** pp) {
    if (g_cfg.reservedAsCommitted && d && pp && !IsInternal() && Readable(d, sizeof *d) && d->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        D3D12_RESOURCE_DESC cd = *d; cd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        HRESULT hr2 = o_CreateCommittedResource(t, &hp, D3D12_HEAP_FLAG_NONE, &cd, s, cv, r, pp);
        static volatile LONG nl; if (InterlockedIncrement(&nl) <= 40)
            Log("[reserved->committed] %s %llux%u mips=%u %s flags=0x%x -> %s res=%p", DimName(d->Dimension), (unsigned long long)d->Width, d->Height, d->MipLevels, FormatName(d->Format), d->Flags, HrName(hr2), SUCCEEDED(hr2) ? *pp : nullptr);
        if (SUCCEEDED(hr2)) {
            InterlockedIncrement(&c_res);
            TrackRes("CreateReservedResource (tiled->committed)", hr2, &hp, 0, &cd, pp);
            LONG k = InterlockedIncrement(&g_substCount) - 1; if (k < 256) g_substRes[k] = *pp;
            { D3D12_RESOURCE_ALLOCATION_INFO ai; t->GetResourceAllocationInfo(&ai, 0, 1, &cd); InterlockedAdd64(&g_substBytes, (LONG64)ai.SizeInBytes); InterlockedIncrement(&g_substN); }
            return hr2;
        }
    }
    HRESULT hr = o_CreateReservedResource(t, d, s, cv, r, pp);
    InterlockedIncrement(&c_res);
    AddCrumb("ID3D12Device::CreateReservedResource", hr);
    TrackRes("CreateReservedResource (tiled)", hr, nullptr, 0, d, pp);
    if (SUCCEEDED(hr) && g_cfg.tileBacking && pp && *pp && d && !IsInternal()) {
        ID3D12Resource* res = nullptr;
        if (SUCCEEDED(((IUnknown*)*pp)->QueryInterface(IID_ID3D12Resource, (void**)&res)) && res) {
            TileCsInit(); EnterCriticalSection(&g_tileCs);
            if (g_pendTiledCount < 64) { g_pendTiled[g_pendTiledCount++] = { t, res, *d, false }; res = nullptr; }
            LeaveCriticalSection(&g_tileCs);
            if (res) res->Release();   // list full
        }
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateFence(ID3D12Device* t, UINT64 v, D3D12_FENCE_FLAGS f, REFIID r, void** pp) {
    HRESULT hr = o_CreateFence(t, v, f, r, pp);
    AddCrumb("ID3D12Device::CreateFence", hr);
    if (FAILED(hr) && FailBudget()) { InterlockedIncrement(&c_fail); Log("!! CreateFence(flags=0x%x) FAILED %s", f, HrName(hr)); }
    return hr;
}

static void OnDeviceRemoved(ID3D12Device* dev, HRESULT reason, const char* where);

static HRESULT STDMETHODCALLTYPE H_GetDeviceRemovedReason(ID3D12Device* t) {
    HRESULT hr = o_GetDeviceRemovedReason(t);
    if (FAILED(hr) && !IsInternal()) OnDeviceRemoved(t, hr, "game called GetDeviceRemovedReason");
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateQueryHeap(ID3D12Device* t, const D3D12_QUERY_HEAP_DESC* d, REFIID r, void** pp) {
    HRESULT hr = o_CreateQueryHeap(t, d, r, pp);
    AddCrumb("ID3D12Device::CreateQueryHeap", hr);
    if (FAILED(hr) && FailBudget()) { InterlockedIncrement(&c_fail); Log("!! CreateQueryHeap(type=%d count=%u) FAILED %s", d ? d->Type : -1, d ? d->Count : 0, HrName(hr)); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateCommandSignature(ID3D12Device* t, const D3D12_COMMAND_SIGNATURE_DESC* d, ID3D12RootSignature* rs, REFIID r, void** pp) {
    HRESULT hr = o_CreateCommandSignature(t, d, rs, r, pp);
    AddCrumb("ID3D12Device::CreateCommandSignature", hr);
    if (FAILED(hr) && FailBudget()) {
        InterlockedIncrement(&c_fail);
        char args[256] = ""; size_t k = 0;
        for (UINT i = 0; d && d->pArgumentDescs && i < d->NumArgumentDescs && i < 16 && k < sizeof args; i++) k += snprintf(args + k, sizeof args - k, "%s%d", i ? "," : "", d->pArgumentDescs[i].Type);
        Log("!! CreateCommandSignature(stride=%u args=[%s] rootsig=%p) FAILED %s (arg types: 0=Draw 1=DrawIndexed 2=Dispatch 6=Constant 7=CBV 8=SRV 9=UAV 11=DispatchRays 12=DispatchMesh)",
            d ? d->ByteStride : 0, args, (void*)rs, HrName(hr));
    }
    return hr;
}

// Pipeline-state stream walker (layout as in d3dx12's CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT)
static bool SubobjectInner(int type, size_t* size, size_t* align) {
#define S(T) *size = sizeof(T); *align = alignof(T); return true
    switch (type) {
    case 0: S(ID3D12RootSignature*);
    case 1: case 2: case 3: case 4: case 5: case 6: case 24: case 25: S(D3D12_SHADER_BYTECODE);
    case 7: S(D3D12_STREAM_OUTPUT_DESC);
    case 8: S(D3D12_BLEND_DESC);
    case 9: S(UINT);
    case 10: S(D3D12_RASTERIZER_DESC);
    case 11: S(D3D12_DEPTH_STENCIL_DESC);
    case 12: S(D3D12_INPUT_LAYOUT_DESC);
    case 13: S(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
    case 14: S(D3D12_PRIMITIVE_TOPOLOGY_TYPE);
    case 15: S(D3D12_RT_FORMAT_ARRAY);
    case 16: S(DXGI_FORMAT);
    case 17: S(DXGI_SAMPLE_DESC);
    case 18: S(UINT);
    case 19: S(D3D12_CACHED_PIPELINE_STATE);
    case 20: S(D3D12_PIPELINE_STATE_FLAGS);
    case 21: S(D3D12_DEPTH_STENCIL_DESC1);
    case 22: S(D3D12_VIEW_INSTANCING_DESC);
    case 26: S(D3D12_DEPTH_STENCIL_DESC2);
    case 27: S(D3D12_RASTERIZER_DESC1);
    case 28: S(D3D12_RASTERIZER_DESC2);
    case 29: S(D3D12_SERIALIZED_ROOT_SIGNATURE_DESC);
    }
    return false;
#undef S
}

static void LogPsoStream(const D3D12_PIPELINE_STATE_STREAM_DESC* sd) {
    if (!sd || !sd->pPipelineStateSubobjectStream || !Readable(sd->pPipelineStateSubobjectStream, sd->SizeInBytes)) return;
    const uint8_t* b = (const uint8_t*)sd->pPipelineStateSubobjectStream;
    size_t off = 0;
    char types[512]; size_t k = 0; types[0] = 0;
    static const char* stageNames[] = { "", "vs", "ps", "ds", "hs", "gs", "cs" };
    while (off + 4 <= sd->SizeInBytes) {
        int type = *(const int*)(b + off);
        size_t sz, al;
        if (!SubobjectInner(type, &sz, &al)) { Log("    (unknown subobject type %d at offset %zu, stop)", type, off); break; }
        const char* tn = EnumLookup(kSubobjNames, type);
        if (k < sizeof types) k += snprintf(types + k, sizeof types - k, "%s%s", k ? "," : "", tn ? tn : "?");
        size_t inner = (off + 4 + al - 1) & ~(al - 1);
        if (inner + sz > sd->SizeInBytes) break;
        const uint8_t* p = b + inner;
        if ((type >= 1 && type <= 6) || type == 24 || type == 25) {
            const char* st = type == 24 ? "as" : type == 25 ? "ms" : stageNames[type];
            LogShader("pso", st, *(const D3D12_SHADER_BYTECODE*)p);
        } else if (type == 15) {
            const D3D12_RT_FORMAT_ARRAY* rt = (const D3D12_RT_FORMAT_ARRAY*)p;
            Log("    RTs=%u [%s,%s,%s]", rt->NumRenderTargets, FormatName(rt->RTFormats[0]), FormatName(rt->RTFormats[1]), FormatName(rt->RTFormats[2]));
        } else if (type == 16) {
            Log("    DSV=%s", FormatName(*(const DXGI_FORMAT*)p));
        }
        off = (inner + sz + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
    }
    Log("    subobjects: %s", types);
}

static HRESULT STDMETHODCALLTYPE H_CreatePipelineState(ID3D12Device* t, const D3D12_PIPELINE_STATE_STREAM_DESC* d, REFIID r, void** pp) {
    HRESULT hr = o_CreatePipelineState(t, d, r, pp);
    InterlockedIncrement(&c_pso);
    AddCrumb("ID3D12Device2::CreatePipelineState", hr);
    if (SUCCEEDED(hr) && pp && *pp) PsoAddStream(*pp, d);
    if (FAILED(hr) && !IsInternal()) {
        InterlockedIncrement(&c_fail);
        if (FailBudget()) { Log("!! ID3D12Device2::CreatePipelineState(stream %zu bytes) FAILED %s", d ? (size_t)d->SizeInBytes : (size_t)0, HrName(hr)); LogPsoStream(d); }
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateCommittedResource1(ID3D12Device* t, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* cv, ID3D12ProtectedResourceSession* ps, REFIID r, void** pp) {
    HRESULT hr = o_CreateCommittedResource1(t, hp, hf, d, s, cv, ps, r, pp);
    InterlockedIncrement(&c_res);
    AddCrumb("ID3D12Device4::CreateCommittedResource1", hr);
    TrackRes("CreateCommittedResource1", hr, hp, hf, d, pp);
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateHeap1(ID3D12Device* t, const D3D12_HEAP_DESC* d, ID3D12ProtectedResourceSession* ps, REFIID r, void** pp) {
    HRESULT hr = o_CreateHeap1(t, d, ps, r, pp);
    InterlockedIncrement(&c_heap);
    if (SUCCEEDED(hr)) RecordHeap(pp, d);
    AddCrumb("ID3D12Device4::CreateHeap1", hr);
    if (FAILED(hr) && FailBudget()) { InterlockedIncrement(&c_fail); Log("!! CreateHeap1(%llu bytes, %s, flags=0x%x) FAILED %s", d ? (unsigned long long)d->SizeInBytes : 0ull, d ? HeapTypeName(d->Properties.Type) : "-", d ? d->Flags : 0, HrName(hr)); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateStateObject(ID3D12Device* t, const D3D12_STATE_OBJECT_DESC* d, REFIID r, void** pp) {
    HRESULT hr = o_CreateStateObject(t, d, r, pp);
    AddCrumb("ID3D12Device5::CreateStateObject", hr);
    if (!IsInternal()) Log("%sID3D12Device5::CreateStateObject(type=%d (0=collection 1=raytracing 2=executable/work-graph), %u subobjects) -> %s",
                           FAILED(hr) ? "!! " : "", d ? d->Type : -1, d ? d->NumSubobjects : 0, HrName(hr));
    if (FAILED(hr)) InterlockedIncrement(&c_fail);
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateCommittedResource2(ID3D12Device* t, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC1* d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* cv, ID3D12ProtectedResourceSession* ps, REFIID r, void** pp) {
    HRESULT hr = o_CreateCommittedResource2(t, hp, hf, d, s, cv, ps, r, pp);
    InterlockedIncrement(&c_res);
    AddCrumb("ID3D12Device8::CreateCommittedResource2", hr);
    TrackRes("CreateCommittedResource2", hr, hp, hf, (const D3D12_RESOURCE_DESC*)d, pp);
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreatePlacedResource1(ID3D12Device* t, ID3D12Heap* h, UINT64 off, const D3D12_RESOURCE_DESC1* d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* cv, REFIID r, void** pp) {
    HRESULT hr = o_CreatePlacedResource1(t, h, off, d, s, cv, r, pp);
    InterlockedIncrement(&c_res);
    AddCrumb("ID3D12Device8::CreatePlacedResource1", hr);
    { int hf2 = 0; const D3D12_HEAP_PROPERTIES* hp2 = PlacedHeapProps(h, &hf2); TrackRes("CreatePlacedResource1", hr, hp2, hf2, (const D3D12_RESOURCE_DESC*)d, pp); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateCommittedResource3(ID3D12Device* t, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC1* d, D3D12_BARRIER_LAYOUT l, const D3D12_CLEAR_VALUE* cv, ID3D12ProtectedResourceSession* ps, UINT32 nf, const DXGI_FORMAT* cf, REFIID r, void** pp) {
    HRESULT hr = o_CreateCommittedResource3(t, hp, hf, d, l, cv, ps, nf, cf, r, pp);
    InterlockedIncrement(&c_res);
    AddCrumb("ID3D12Device10::CreateCommittedResource3", hr);
    TrackRes("CreateCommittedResource3 (enhanced barriers)", hr, hp, hf, (const D3D12_RESOURCE_DESC*)d, pp);
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreatePlacedResource2(ID3D12Device* t, ID3D12Heap* h, UINT64 off, const D3D12_RESOURCE_DESC1* d, D3D12_BARRIER_LAYOUT l, const D3D12_CLEAR_VALUE* cv, UINT32 nf, const DXGI_FORMAT* cf, REFIID r, void** pp) {
    HRESULT hr = o_CreatePlacedResource2(t, h, off, d, l, cv, nf, cf, r, pp);
    InterlockedIncrement(&c_res);
    AddCrumb("ID3D12Device10::CreatePlacedResource2", hr);
    { int hf2 = 0; const D3D12_HEAP_PROPERTIES* hp2 = PlacedHeapProps(h, &hf2); TrackRes("CreatePlacedResource2 (enhanced barriers)", hr, hp2, hf2, (const D3D12_RESOURCE_DESC*)d, pp); }
    return hr;
}

static bool PatchSlot(void** vt, int idx, void* hook, void** orig);
typedef void (STDMETHODCALLTYPE *PFN_ECL)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_QSignal)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
typedef void (STDMETHODCALLTYPE *PFN_UTM)(ID3D12CommandQueue*, ID3D12Resource*, UINT, const D3D12_TILED_RESOURCE_COORDINATE*, const D3D12_TILE_REGION_SIZE*,
                                          ID3D12Heap*, UINT, const D3D12_TILE_RANGE_FLAGS*, const UINT*, const UINT*, D3D12_TILE_MAPPING_FLAGS);
typedef void (STDMETHODCALLTYPE *PFN_CTM)(ID3D12CommandQueue*, ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*, ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*, const D3D12_TILE_REGION_SIZE*, D3D12_TILE_MAPPING_FLAGS);
typedef void (STDMETHODCALLTYPE *PFN_RemoveDevice)(ID3D12Device*);
static PFN_ECL o_ExecuteCommandLists; static PFN_QSignal o_QSignal, o_QWait; static PFN_UTM o_UpdateTileMappings; static PFN_CTM o_CopyTileMappings;
static PFN_RemoveDevice o_RemoveDevice;
static void** g_hookedQueueVtbl;
static void DescribeValue(const char* where, DWORD64 v);

static void TileBackingDrain(ID3D12CommandQueue* q) {
    PendingTiled todo[64]; int n = 0;
    EnterCriticalSection(&g_tileCs);
    for (LONG i = 0; i < g_pendTiledCount; i++) todo[n++] = g_pendTiled[i];
    g_pendTiledCount = 0;
    LeaveCriticalSection(&g_tileCs);
    InternalScope guard;
    for (int i = 0; i < n; i++) {
        PendingTiled& p = todo[i];
        if (p.skip) { Log("[tilebacking] %p: game maps it itself - skipped", (void*)p.res); p.res->Release(); continue; }
        UINT tiles = 0, sub = 0; D3D12_PACKED_MIP_INFO pk = {}; D3D12_TILE_SHAPE shape = {};
        p.dev->GetResourceTiling(p.res, &tiles, &pk, &shape, &sub, 0, nullptr);
        if (!tiles) { Log("[tilebacking] %p: GetResourceTiling returned 0 tiles", (void*)p.res); p.res->Release(); continue; }
        D3D12_HEAP_DESC hd = {};
        hd.SizeInBytes = (UINT64)tiles * 65536; hd.Alignment = 65536; hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        const D3D12_RESOURCE_FLAGS f = p.desc.Flags;
        if (p.desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
        else if (f & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
        else hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
        ID3D12Heap* heap = nullptr;
        HRESULT hr = o_CreateHeap(p.dev, &hd, IID_ID3D12Heap, (void**)&heap);
        if (FAILED(hr) || !heap) { Log("!! [tilebacking] CreateHeap(%llu bytes) FAILED %s", (unsigned long long)hd.SizeInBytes, HrName(hr)); p.res->Release(); continue; }
        D3D12_TILED_RESOURCE_COORDINATE co = {}; D3D12_TILE_REGION_SIZE rs = {}; rs.NumTiles = tiles; rs.UseBox = FALSE;
        D3D12_TILE_RANGE_FLAGS rf = D3D12_TILE_RANGE_FLAG_NONE; UINT hs = 0, cnt = tiles;
        o_UpdateTileMappings(q, p.res, 1, &co, &rs, heap, 1, &rf, &hs, &cnt, D3D12_TILE_MAPPING_FLAG_NONE);
        InterlockedIncrement(&c_tileBacked);
        Log("[tilebacking] mapped %u tiles (%llu KB, heapFlags=0x%x) into %p  %s %llux%u fmt=%u flags=0x%x",
            tiles, (unsigned long long)(hd.SizeInBytes / 1024), (unsigned)hd.Flags, (void*)p.res,
            p.desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? "BUFFER" : "TEX", (unsigned long long)p.desc.Width, p.desc.Height, (unsigned)p.desc.Format, (unsigned)f);
        heap->Release();   // the mapping keeps the heap alive
        p.res->Release();
    }
}

static LONG g_syncCount;
static void SyncAfterSubmit(ID3D12CommandQueue* q, int t, UINT n, ID3D12CommandList* const* lists) {
    LONG idx = InterlockedIncrement(&g_syncCount);
    if (idx > 60) { g_cfg.syncSubmits = 0; Log("[sync] 60 submits checked - sync disabled"); return; }
    InternalScope guard;
    ID3D12Device* dev = nullptr;
    if (FAILED(q->GetDevice(IID_ID3D12Device, (void**)&dev)) || !dev) return;
    ID3D12Fence* f = nullptr; HRESULT hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence, (void**)&f);
    DWORD t0 = GetTickCount(); const char* res = "ok";
    if (SUCCEEDED(hr) && f) {
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        o_QSignal(q, f, 1);
        f->SetEventOnCompletion(1, ev);
        DWORD w = WaitForSingleObject(ev, 3000);
        if (w != WAIT_OBJECT_0) { res = "TIMEOUT(3s)"; g_cfg.syncSubmits = 0; }
        CloseHandle(ev); f->Release();
    }
    HRESULT rr = dev->GetDeviceRemovedReason();
    char lt[96] = "";
    for (UINT i = 0; lists && i < n && i < 4; i++) {
        ID3D12CommandList* cl = lists[i]; D3D12_COMMAND_LIST_TYPE ty = cl ? cl->GetType() : (D3D12_COMMAND_LIST_TYPE)-1;
        size_t l = strlen(lt); snprintf(lt + l, sizeof(lt) - l, " %p(t%d)", (void*)cl, (int)ty);
    }
    if (t == D3D12_COMMAND_LIST_TYPE_DIRECT && g_cfg.recordLists) { static LONG nd; if (InterlockedIncrement(&nd) <= 4) for (UINT i = 0; lists && i < n && i < 4; i++) DumpListOps(lists[i], "direct submit (for comparison)"); }
    Log("[sync] #%ld %s queue=%p lists=%u:%s -> gpu %s in %lu ms, deviceRemoved=%s", idx, QTypeName(t), (void*)q, n, lt, res, GetTickCount() - t0, rr == S_OK ? "no" : HrName(rr));
    if (FAILED(rr)) {
        for (UINT i = 0; lists && i < n && i < 4; i++) DumpListOps(lists[i], "list in the submit that hung the GPU");
        OnDeviceRemoved(dev, rr, "SyncSubmits: found right after this submit");
    }
    dev->Release();
}

static void STDMETHODCALLTYPE H_ExecuteCommandLists(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    int t = QueueType(q);
    if (t < 0) {
        D3D12_COMMAND_QUEUE_DESC qd = {};
#if defined(_MSC_VER) || !defined(_WIN32)
        qd = q->GetDesc();
#else
        q->GetDesc(&qd);
#endif
        LONG i = InterlockedIncrement(&g_queueCount) - 1;
        if (i < 32) { g_queues[i].q = q; g_queues[i].type = qd.Type; }
        t = qd.Type;
        Log("Queue %p (created outside the device hooks) identified as %s", (void*)q, QTypeName(t));
    }
    static const char* names[5] = { "Queue::ExecuteCommandLists DIRECT", "Queue::ExecuteCommandLists BUNDLE", "Queue::ExecuteCommandLists COMPUTE", "Queue::ExecuteCommandLists COPY", "Queue::ExecuteCommandLists ?" };
    LONG c = (t >= 0 && t < 4) ? InterlockedIncrement(&c_exec[t]) : 0;
    if (c && c <= 3) Log("%s #%ld (%u lists, queue %p)", names[t], c, n, (void*)q);
    if (g_cfg.recordLists) for (UINT i = 0; i < n && lists && i < 8; i++) if (lists[i]) HookListVtbl(lists[i]);
    if (t == D3D12_COMMAND_LIST_TYPE_DIRECT && g_cfg.tileBacking && g_pendTiledCount > 0) TileBackingDrain(q);
    o_ExecuteCommandLists(q, n, lists);
    AddCrumb(names[(t >= 0 && t < 4) ? t : 4], S_OK);
    if (g_cfg.syncSubmits && !IsInternal()) SyncAfterSubmit(q, t, n, lists);
}
static HRESULT STDMETHODCALLTYPE H_QSignal(ID3D12CommandQueue* q, ID3D12Fence* f, UINT64 v) {
    HRESULT hr = o_QSignal(q, f, v); InterlockedIncrement(&c_signal);
    AddCrumb("Queue::Signal", hr);
    if (FAILED(hr) && FailBudget()) Log("!! Queue(%s)::Signal(%llu) FAILED %s", QTypeName(QueueType(q)), (unsigned long long)v, HrName(hr));
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_QWait(ID3D12CommandQueue* q, ID3D12Fence* f, UINT64 v) {
    HRESULT hr = o_QWait(q, f, v); InterlockedIncrement(&c_wait);
    AddCrumb("Queue::Wait", hr);
    if (FAILED(hr) && FailBudget()) Log("!! Queue(%s)::Wait(%llu) FAILED %s", QTypeName(QueueType(q)), (unsigned long long)v, HrName(hr));
    return hr;
}
static void STDMETHODCALLTYPE H_UpdateTileMappings(ID3D12CommandQueue* q, ID3D12Resource* res, UINT nReg, const D3D12_TILED_RESOURCE_COORDINATE* coords, const D3D12_TILE_REGION_SIZE* sizes,
                                                   ID3D12Heap* heap, UINT nRanges, const D3D12_TILE_RANGE_FLAGS* rf, const UINT* heapStart, const UINT* counts, D3D12_TILE_MAPPING_FLAGS mf) {
    LONG c = InterlockedIncrement(&c_tile);
    if (res && IsSubst(res)) { Log("Queue::UpdateTileMappings on a substituted (committed) resource %p ignored", (void*)res); return; }
    if (g_cfg.tileBacking && g_tileCsInit && res) {
        EnterCriticalSection(&g_tileCs);
        for (LONG i = 0; i < g_pendTiledCount; i++) if (g_pendTiled[i].res == res) g_pendTiled[i].skip = true;
        LeaveCriticalSection(&g_tileCs);
    }
    if (c <= 40) {
        int nullRanges = 0, skip = 0, reuse = 0; UINT tiles = 0;
        for (UINT i = 0; rf && i < nRanges && i < 4096; i++) { if (rf[i] & D3D12_TILE_RANGE_FLAG_NULL) nullRanges++; if (rf[i] & D3D12_TILE_RANGE_FLAG_SKIP) skip++; if (rf[i] & D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE) reuse++; }
        for (UINT i = 0; counts && i < nRanges && i < 4096; i++) tiles += counts[i];
        HeapRec* hr = HeapSlot(heap, false);
        Log("Queue(%s)::UpdateTileMappings #%ld res=%p regions=%u ranges=%u (null=%d skip=%d reuseSingle=%d) tiles=%u heap=%p(%s) flags=0x%x",
            QTypeName(QueueType(q)), c, (void*)res, nReg, nRanges, nullRanges, skip, reuse, tiles, (void*)heap, hr ? HeapTypeName(hr->type) : (heap ? "?" : "none"), mf);
        if (c <= 6) DescribeValue("tiled", (DWORD64)res);
    }
    o_UpdateTileMappings(q, res, nReg, coords, sizes, heap, nRanges, rf, heapStart, counts, mf);
    AddCrumb("Queue::UpdateTileMappings", S_OK);
}
static void STDMETHODCALLTYPE H_CopyTileMappings(ID3D12CommandQueue* q, ID3D12Resource* dst, const D3D12_TILED_RESOURCE_COORDINATE* dc, ID3D12Resource* src, const D3D12_TILED_RESOURCE_COORDINATE* sc, const D3D12_TILE_REGION_SIZE* sz, D3D12_TILE_MAPPING_FLAGS f) {
    Log("Queue(%s)::CopyTileMappings dst=%p src=%p", QTypeName(QueueType(q)), (void*)dst, (void*)src);
    if (IsSubst(dst) || IsSubst(src)) return;
    o_CopyTileMappings(q, dst, dc, src, sc, sz, f);
    AddCrumb("Queue::CopyTileMappings", S_OK);
}
static void STDMETHODCALLTYPE H_RemoveDevice(ID3D12Device* d) {
    InterlockedIncrement(&c_removeDevice);
    Log("!! GAME CALLED ID3D12Device5::RemoveDevice() - the game removed the device itself. Caller stack:");
    CONTEXT c; RtlCaptureContext(&c);
    WalkStack(&c, 24);
    LogCrumbs();
    o_RemoveDevice(d);
}
static void HookQueue(void* pq) {
    ID3D12CommandQueue* q = nullptr;
    if (FAILED(((IUnknown*)pq)->QueryInterface(IID_ID3D12CommandQueue, (void**)&q)) || !q) return;
    void** vt = *(void***)q;
    if (!g_hookedQueueVtbl) {
        g_hookedQueueVtbl = vt;
        PatchSlot(vt, IDX_Queue_ExecuteCommandLists, (void*)&H_ExecuteCommandLists, (void**)&o_ExecuteCommandLists);
        PatchSlot(vt, IDX_Queue_Signal, (void*)&H_QSignal, (void**)&o_QSignal);
        PatchSlot(vt, IDX_Queue_Wait, (void*)&H_QWait, (void**)&o_QWait);
        PatchSlot(vt, IDX_Queue_UpdateTileMappings, (void*)&H_UpdateTileMappings, (void**)&o_UpdateTileMappings);
        PatchSlot(vt, IDX_Queue_CopyTileMappings, (void*)&H_CopyTileMappings, (void**)&o_CopyTileMappings);
        Log("Command queue vtable %p hooked", (void*)vt);
    } else if (vt != g_hookedQueueVtbl) Log("Queue %p has a different vtable - not hooked", (void*)q);
    q->Release();
}

static bool PatchSlot(void** vt, int idx, void* hook, void** orig) {
    if (vt[idx] == hook) return true;
    DWORD old;
    if (!VirtualProtect(&vt[idx], sizeof(void*), PAGE_READWRITE, &old)) return false;
    *orig = vt[idx];
    InterlockedExchangePointer(&vt[idx], hook);
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
    return true;
}

static void HookDeviceVtable(ID3D12Device* dev) {
    void** vt = *(void***)dev;
    if (g_hookedDeviceVtbl == vt) return;
    if (g_hookedDeviceVtbl) { Log("Device %p uses a different vtable (%p) than the hooked one - not hooking it (debug layer / second runtime?)", (void*)dev, (void*)vt); return; }
    g_hookedDeviceVtbl = vt;
    int n = 0;
#define HOOK(name, idx) n += PatchSlot(vt, idx, (void*)&H_##name, (void**)&o_##name)
    HOOK(CreateCommandQueue, IDX_Device_CreateCommandQueue);
    HOOK(CreateCommandAllocator, IDX_Device_CreateCommandAllocator);
    HOOK(CreateGraphicsPipelineState, IDX_Device_CreateGraphicsPipelineState);
    HOOK(CreateComputePipelineState, IDX_Device_CreateComputePipelineState);
    HOOK(CreateCommandList, IDX_Device_CreateCommandList);
    HOOK(CheckFeatureSupport, IDX_Device_CheckFeatureSupport);
    HOOK(CreateDescriptorHeap, IDX_Device_CreateDescriptorHeap);
    HOOK(CreateRootSignature, IDX_Device_CreateRootSignature);
    HOOK(CreateCommittedResource, IDX_Device_CreateCommittedResource);
    HOOK(CreateHeap, IDX_Device_CreateHeap);
    HOOK(CreatePlacedResource, IDX_Device_CreatePlacedResource);
    HOOK(CreateReservedResource, IDX_Device_CreateReservedResource);
    HOOK(CreateFence, IDX_Device_CreateFence);
    HOOK(CreateRenderTargetView, IDX_Device_CreateRenderTargetView);
    HOOK(CreateDepthStencilView, IDX_Device_CreateDepthStencilView);
    HOOK(CopyDescriptors, IDX_Device_CopyDescriptors);
    HOOK(CopyDescriptorsSimple, IDX_Device_CopyDescriptorsSimple);
    HOOK(GetDeviceRemovedReason, IDX_Device_GetDeviceRemovedReason);
    HOOK(CreateQueryHeap, IDX_Device_CreateQueryHeap);
    HOOK(CreateCommandSignature, IDX_Device_CreateCommandSignature);
    IUnknown* q = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_ID3D12Device2, (void**)&q))) { q->Release(); HOOK(CreatePipelineState, IDX_Device2_CreatePipelineState); }
    if (SUCCEEDED(dev->QueryInterface(IID_ID3D12Device4, (void**)&q))) { q->Release(); HOOK(CreateCommittedResource1, IDX_Device4_CreateCommittedResource1); HOOK(CreateHeap1, IDX_Device4_CreateHeap1); HOOK(CreateCommandList1, IDX_Device4_CreateCommandList1); }
    if (SUCCEEDED(dev->QueryInterface(IID_ID3D12Device5, (void**)&q))) { q->Release(); HOOK(CreateStateObject, IDX_Device5_CreateStateObject); HOOK(RemoveDevice, IDX_Device5_RemoveDevice); }
    if (SUCCEEDED(dev->QueryInterface(IID_ID3D12Device8, (void**)&q))) { q->Release(); HOOK(CreateCommittedResource2, IDX_Device8_CreateCommittedResource2); HOOK(CreatePlacedResource1, IDX_Device8_CreatePlacedResource1); }
    if (SUCCEEDED(dev->QueryInterface(IID_ID3D12Device9, (void**)&q))) { q->Release(); HOOK(CreateCommandQueue1, IDX_Device9_CreateCommandQueue1); }
    if (SUCCEEDED(dev->QueryInterface(IID_ID3D12Device10, (void**)&q))) { q->Release(); HOOK(CreateCommittedResource3, IDX_Device10_CreateCommittedResource3); HOOK(CreatePlacedResource2, IDX_Device10_CreatePlacedResource2); }
#undef HOOK
    Log("Device vtable %p hooked (%d methods)", (void*)vt, n);
}

// =============================================================================
//  Device-removed / DRED
// =============================================================================
static void DumpDred(ID3D12Device* dev) {
    ID3D12DeviceRemovedExtendedData* dred = nullptr;
    if (FAILED(dev->QueryInterface(IID_ID3D12DeviceRemovedExtendedData, (void**)&dred)) || !dred) { Log("  DRED: not available on this device"); return; }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc = {};
    HRESULT hr = dred->GetAutoBreadcrumbsOutput(&bc);
    if (FAILED(hr)) Log("  DRED breadcrumbs: %s (EnableDRED=1 must be set before the device is created)", HrName(hr));
    else {
        int nodes = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE* n = bc.pHeadAutoBreadcrumbNode; n && nodes < 64; n = n->pNext, nodes++) {
            UINT last = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
            bool incomplete = last < n->BreadcrumbCount;
            Log("  DRED %s queue=\"%s\" list=\"%s\" completed %u/%u ops", incomplete ? "!! INCOMPLETE" : "ok",
                n->pCommandQueueDebugNameA ? n->pCommandQueueDebugNameA : (n->pCommandQueueDebugNameW ? U8(n->pCommandQueueDebugNameW).b : "?"),
                n->pCommandListDebugNameA ? n->pCommandListDebugNameA : (n->pCommandListDebugNameW ? U8(n->pCommandListDebugNameW).b : "?"),
                last, n->BreadcrumbCount);
            if (incomplete && n->pCommandHistory) {
                UINT from = last > 6 ? last - 6 : 0, to = last + 6 < n->BreadcrumbCount ? last + 6 : n->BreadcrumbCount;
                for (UINT i = from; i < to; i++) {
                    const char* op = EnumLookup(kBreadcrumbOpNames, n->pCommandHistory[i]);
                    Log("      %s op[%u] %s", i == last ? "-->" : "   ", i, op ? op : "?");
                }
            }
        }
        if (!nodes) Log("  DRED breadcrumbs: no command lists recorded");
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT pf = {};
    hr = dred->GetPageFaultAllocationOutput(&pf);
    if (SUCCEEDED(hr)) {
        Log("  DRED page fault VA: 0x%016llx%s", (unsigned long long)pf.PageFaultVA, pf.PageFaultVA ? "" : " (none - not a page fault)");
        int c = 0;
        for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadExistingAllocationNode; a && c < 16; a = a->pNext, c++)
            Log("    existing allocation at VA: \"%s\" type=%s", a->ObjectNameA ? a->ObjectNameA : (a->ObjectNameW ? U8(a->ObjectNameW).b : "?"), EnumLookup(kDredAllocNames, a->AllocationType) ? EnumLookup(kDredAllocNames, a->AllocationType) : "?");
        c = 0;
        for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadRecentFreedAllocationNode; a && c < 16; a = a->pNext, c++)
            Log("    recently FREED allocation at VA: \"%s\" type=%s", a->ObjectNameA ? a->ObjectNameA : (a->ObjectNameW ? U8(a->ObjectNameW).b : "?"), EnumLookup(kDredAllocNames, a->AllocationType) ? EnumLookup(kDredAllocNames, a->AllocationType) : "?");
    }
    dred->Release();
}

static void OnDeviceRemoved(ID3D12Device* dev, HRESULT reason, const char* where) {
    if (InterlockedExchange(&g_deviceRemovedReported, 1)) return;
    InternalScope is;
    Log("==================== GPU DEVICE REMOVED ====================");
    Log("  Detected by: %s", where);
    Log("  Reason: %s", HrName(reason));
    switch ((uint32_t)reason) {
    case 0x887A0006: Log("  (DEVICE_HUNG: the GPU took too long on a command - TDR. Often an unsupported feature/shader on an older GPU)"); break;
    case 0x887A0005: Log("  (DEVICE_REMOVED: driver crash/upgrade or GPU fault)"); break;
    case 0x887A0007: Log("  (DEVICE_RESET: badly formed commands sent by the game)"); break;
    case 0x887A0020: Log("  (DRIVER_INTERNAL_ERROR: the driver hit an internal error)"); break;
    case 0x8007000E: Log("  (E_OUTOFMEMORY: ran out of video memory)"); break;
    }
    DumpDred(dev);
    PragRelocProbe("device removed");
    LogCrumbs();
    WriteMinidump("device_removed", nullptr);
    Log("==================== end of device-removed report ====================");
}

// =============================================================================
//  Device creation
// =============================================================================
static void LogAdapter(IUnknown* a, char* out, size_t n) {
    out[0] = 0;
    if (!a) { snprintf(out, n, "default adapter"); return; }
    IDXGIAdapter1* ad = nullptr;
    if (SUCCEEDED(a->QueryInterface(IID_IDXGIAdapter1, (void**)&ad)) && ad) {
        DXGI_ADAPTER_DESC1 d; if (SUCCEEDED(ad->GetDesc1(&d))) snprintf(out, n, "\"%s\" %04x:%04x", U8(d.Description).b, d.VendorId, d.DeviceId);
        ad->Release();
    }
    if (!out[0]) snprintf(out, n, "%p", (void*)a);
}

// =============================================================================
//  In-memory game patches (signature checked, applied once the exe is unpacked)
// =============================================================================
// RE Engine (Pragmata): GPU buffers created with "remap" flag (desc flags bit 29) are
// translated through a table: RemapTable_Find(table, res). If res isn't in the table
// the game dereferences NULL (+0x18) and crashes. Patch: not found -> return res itself
// (the same thing the function does when the table is disabled).
//   48 85 C0 75 DB | 4C 8B D0 | 49 8B 42 18 | C3 | 48 8B C2 | C3 | CC CC CC CC CC
//                    mov r10,rax  mov rax,[r10+18] ret  mov rax,rdx ret  padding
// becomes          ... mov r10,rax | test r10,r10 ; jnz +4 | mov rax,rdx ; ret | mov rax,[r10+18] ; ret
static const uint8_t kRemapSig[]   = { 0x48,0x85,0xC0,0x75,0xDB,0x4C,0x8B,0xD0,0x49,0x8B,0x42,0x18,0xC3,0x48,0x8B,0xC2,0xC3,0xCC,0xCC,0xCC,0xCC,0xCC };
static const uint8_t kRemapPatched[] = { 0x48,0x85,0xC0,0x75,0xDB,0x4C,0x8B,0xD0,0x4D,0x85,0xD2,0x75,0x04,0x48,0x8B,0xC2,0xC3,0x49,0x8B,0x42,0x18,0xC3 };

static uint8_t* FindInExe(const uint8_t* sig, size_t n, int* count) {
    HMODULE exe = GetModuleHandleW(nullptr);
    IMAGE_NT_HEADERS64* nt = NtHeaders(exe);
    *count = 0;
    if (!nt) return nullptr;
    uint8_t* found = nullptr;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* p = (uint8_t*)exe + sec[i].VirtualAddress;
        size_t sz = sec[i].Misc.VirtualSize;
        // scan readable pieces only
        for (size_t off = 0; off < sz;) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(p + off, &mbi, sizeof mbi)) break;
            size_t run = (uint8_t*)mbi.BaseAddress + mbi.RegionSize - (p + off);
            if (run > sz - off) run = sz - off;
            bool ok = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
            if (ok && run >= n) {
                uint8_t* q = p + off; uint8_t* end = q + run - n;
                for (; q <= end; q++) {
                    if (q[0] != sig[0] || memcmp(q, sig, n)) continue;
                    if (!found) found = q;
                    (*count)++;
                }
            }
            off += run;
        }
    }
    return found;
}

static DWORD g_writeCodeErr;
static bool WriteCode(uint8_t* at, const uint8_t* bytes, size_t n) {
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) {
        g_writeCodeErr = GetLastError();
        if (!WriteProcessMemory(GetCurrentProcess(), at, bytes, n, nullptr)) return false;  // second attempt
        FlushInstructionCache(GetCurrentProcess(), at, n);
        return memcmp(at, bytes, n) == 0;
    }
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return true;
}

static void LogCodePolicy() {
    typedef BOOL (WINAPI *PFN_GPMP)(HANDLE, PROCESS_MITIGATION_POLICY, PVOID, SIZE_T);
    PFN_GPMP gp = (PFN_GPMP)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcessMitigationPolicy");
    if (!gp) return;
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dc = {};
    if (gp(GetCurrentProcess(), ProcessDynamicCodePolicy, &dc, sizeof dc))
        Log("[patch]   process dynamic-code policy (ACG): ProhibitDynamicCode=%u AllowThreadOptOut=%u AllowRemoteDowngrade=%u",
            (unsigned)dc.ProhibitDynamicCode, (unsigned)dc.AllowThreadOptOut, (unsigned)dc.AllowRemoteDowngrade);
}

static volatile LONG g_patchesDone;
static void ApplyGamePatches(const char* when) {
    if (!g_cfg.patchRemapFallback || g_patchesDone) return;
    int cnt = 0, cntP = 0;
    uint8_t* at = FindInExe(kRemapSig, sizeof kRemapSig, &cnt);
    uint8_t* atP = FindInExe(kRemapPatched, sizeof kRemapPatched, &cntP);
    if (atP && !at) { Log("[patch] RemapFallback already applied at %s", AddrName(atP)); InterlockedExchange(&g_patchesDone, 1); return; }
    if (!at) { Log("[patch] RemapFallback: signature not found (%s) - game version differs or code still packed", when); return; }
    if (cnt != 1) { Log("[patch] RemapFallback: signature found %d times - not patching (ambiguous)", cnt); InterlockedExchange(&g_patchesDone, 1); return; }
    InterlockedExchange(&g_patchesDone, 1);
    bool ok = g_cfg.patchRemapFallback != 2 && WriteCode(at, kRemapPatched, sizeof kRemapPatched);
    if (g_cfg.patchRemapFallback == 2) g_writeCodeErr = 0;
    if (ok) {
        Log("[patch] RemapFallback APPLIED in code at %s (%s): remapped GPU buffers missing from the remap table now use their own resource", AddrName(at), when);
        return;
    }
    // Code can't be written (e.g. Arbitrary Code Guard): handle the fault in the exception handler instead.
    g_remapFaultSite = at + 8;   // "mov rax, [r10+18h]"
    if (g_cfg.patchRemapFallback == 2)
        Log("[patch] RemapFallback: exception-handler fix armed at %s (RemapFallback=2, game code left untouched) (%s)", AddrName(g_remapFaultSite), when);
    else
        Log("[patch] RemapFallback: game code is write-protected (error %lu) -> using the exception-handler fix at %s instead (%s)",
            g_writeCodeErr, AddrName(g_remapFaultSite), when);
    LogCodePolicy();
}

// Once, on the game's first D3D12 call (not in DllMain): GPUs, Agility SDK.
static void LateInit() {
    if (InterlockedExchange(&g_lateInitDone, 1)) return;
    InternalScope is;
    ApplyGamePatches("first D3D12 call");
    LogAgility();
    typedef HRESULT (WINAPI *PFN_CDXGIF1)(REFIID, void**);
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    PFN_CDXGIF1 cf = dxgi ? (PFN_CDXGIF1)(void*)GetProcAddress(dxgi, "CreateDXGIFactory1") : nullptr;
    IDXGIFactory1* f = nullptr;
    if (cf && SUCCEEDED(cf(IID_IDXGIFactory1, (void**)&f)) && f) {
        IDXGIAdapter1* a = nullptr;
        for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND && i < 8; i++) {
            DXGI_ADAPTER_DESC1 d = {}; a->GetDesc1(&d);
            LARGE_INTEGER umd = {};
            char drv[64] = "?";
            if (SUCCEEDED(a->CheckInterfaceSupport(IID_IDXGIDevice, &umd)))
                snprintf(drv, sizeof drv, "%u.%u.%u.%u", HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart), LOWORD(umd.LowPart));
            Log("GPU %u: \"%s\" vendor=%04x device=%04x VRAM=%llu MB shared=%llu MB driver=%s%s", i, U8(d.Description).b, d.VendorId, d.DeviceId,
                (unsigned long long)(d.DedicatedVideoMemory >> 20), (unsigned long long)(d.SharedSystemMemory >> 20), drv,
                (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " (software)" : "");
            a->Release();
        }
        f->Release();
    } else Log("DXGI: could not create a factory to list GPUs");
    if (!g_dbghelp) { wchar_t p[MAX_PATH]; GetSystemDirectoryW(p, MAX_PATH); wcscat(p, L"\\dbghelp.dll"); g_dbghelp = LoadLibraryW(p); }
}

// Once, right before the game's first device is created: probe what each GPU
// supports, enable DRED / debug layer (both must happen before device creation).
static void PreDeviceCreate(ID3D12DeviceFactory* factory) {
    if (InterlockedExchange(&g_preDeviceDone, 1)) return;
    InternalScope is;
    if (r_CreateDevice && !factory) {
        typedef HRESULT (WINAPI *PFN_CDXGIF1)(REFIID, void**);
        HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
        PFN_CDXGIF1 cf = dxgi ? (PFN_CDXGIF1)(void*)GetProcAddress(dxgi, "CreateDXGIFactory1") : nullptr;
        IDXGIFactory1* f = nullptr;
        if (cf && SUCCEEDED(cf(IID_IDXGIFactory1, (void**)&f)) && f) {
            IDXGIAdapter1* a = nullptr;
            for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND && i < 8; i++) {
                static const D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
                const char* best = "none (no D3D12 support)";
                for (D3D_FEATURE_LEVEL fl : fls) {
                    if (SUCCEEDED(r_CreateDevice(a, fl, IID_ID3D12Device, nullptr))) { best = FlName(fl); break; }
                }
                Log("GPU %u: highest D3D12 feature level the driver accepts = %s", i, best);
                a->Release();
            }
            f->Release();
        }
    }
    if (g_cfg.enableDRED && (r_GetDebugInterface || factory)) {
        // a device factory has its own configuration; the global one does not apply to it
        ID3D12DeviceRemovedExtendedDataSettings* s = nullptr;
        HRESULT hr = factory ? factory->GetConfigurationInterface(CLSID_D3D12DeviceRemovedExtendedData, IID_ID3D12DeviceRemovedExtendedDataSettings, (void**)&s)
                             : r_GetDebugInterface(IID_ID3D12DeviceRemovedExtendedDataSettings, (void**)&s);
        if (SUCCEEDED(hr) && s) {
            s->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            s->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            s->Release();
            Log("DRED enabled (auto-breadcrumbs + page-fault reporting)");
        } else Log("DRED could not be enabled: %s", HrName(hr));
    }
    if (g_cfg.debugLayer && (r_GetDebugInterface || factory)) {
        ID3D12Debug* dbg = nullptr;
        HRESULT hr = factory ? factory->GetConfigurationInterface(CLSID_D3D12Debug, IID_ID3D12Debug, (void**)&dbg)
                             : r_GetDebugInterface(IID_ID3D12Debug, (void**)&dbg);
        if (SUCCEEDED(hr) && dbg) { dbg->EnableDebugLayer(); dbg->Release(); Log("D3D12 debug layer enabled"); }
        else Log("D3D12 debug layer NOT available: %s (install Windows optional feature 'Graphics Tools')", HrName(hr));
    }
}

static void CALLBACK InfoQueueCallback(D3D12_MESSAGE_CATEGORY cat, D3D12_MESSAGE_SEVERITY sev, D3D12_MESSAGE_ID id, LPCSTR desc, void*) {
    if (sev > D3D12_MESSAGE_SEVERITY_WARNING || (sev == D3D12_MESSAGE_SEVERITY_WARNING && !g_cfg.debugLayerWarnings)) return;
    static volatile LONG count;
    if (InterlockedIncrement(&count) > 1000) return;
    static const char* sevs[] = { "CORRUPTION", "ERROR", "WARNING", "INFO", "MESSAGE" };
    Log("[debug-layer %s] cat=%d id=%d %s", sev <= 4 ? sevs[sev] : "?", cat, id, desc ? desc : "");
}

static void LogDeviceCaps(ID3D12Device* dev) {
    InternalScope is;
    Log("Device caps (nodes %u):", dev->GetNodeCount());
    static const D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl = { 5, fls, D3D_FEATURE_LEVEL_11_0 };
    char d[700];
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof fl))) Log("  MaxSupportedFeatureLevel = %s", FlName(fl.MaxSupportedFeatureLevel));
    D3D12_FEATURE_DATA_SHADER_MODEL sm = { (D3D_SHADER_MODEL)0x69 };
    while (sm.HighestShaderModel >= 0x51 && FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm)))
        sm.HighestShaderModel = (D3D_SHADER_MODEL)(sm.HighestShaderModel == 0x60 ? 0x51 : sm.HighestShaderModel - 1);
    Log("  HighestShaderModel = %x.%x", (sm.HighestShaderModel >> 4) & 0xF, sm.HighestShaderModel & 0xF);
    D3D12_FEATURE_DATA_ROOT_SIGNATURE rs = { D3D_ROOT_SIGNATURE_VERSION_1_2 };
    if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &rs, sizeof rs))) { rs.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1; dev->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &rs, sizeof rs); }
    Log("  Root signature version = 0x%x", rs.HighestVersion);
#define CAP(F, T) { T x = {}; if (SUCCEEDED(dev->CheckFeatureSupport(F, &x, sizeof x))) { DescribeFeature(F, &x, sizeof x, d, sizeof d); Log("  %s: %s", FeatureName(F), d); } else Log("  %s: not supported by this runtime", FeatureName(F)); }
    CAP(D3D12_FEATURE_D3D12_OPTIONS, D3D12_FEATURE_DATA_D3D12_OPTIONS)
    CAP(D3D12_FEATURE_D3D12_OPTIONS1, D3D12_FEATURE_DATA_D3D12_OPTIONS1)
    CAP(D3D12_FEATURE_ARCHITECTURE1, D3D12_FEATURE_DATA_ARCHITECTURE1)
    CAP(D3D12_FEATURE_D3D12_OPTIONS3, D3D12_FEATURE_DATA_D3D12_OPTIONS3)
    CAP(D3D12_FEATURE_D3D12_OPTIONS4, D3D12_FEATURE_DATA_D3D12_OPTIONS4)
    CAP(D3D12_FEATURE_D3D12_OPTIONS5, D3D12_FEATURE_DATA_D3D12_OPTIONS5)
    CAP(D3D12_FEATURE_D3D12_OPTIONS6, D3D12_FEATURE_DATA_D3D12_OPTIONS6)
    CAP(D3D12_FEATURE_D3D12_OPTIONS7, D3D12_FEATURE_DATA_D3D12_OPTIONS7)
    CAP(D3D12_FEATURE_D3D12_OPTIONS9, D3D12_FEATURE_DATA_D3D12_OPTIONS9)
    CAP(D3D12_FEATURE_D3D12_OPTIONS12, D3D12_FEATURE_DATA_D3D12_OPTIONS12)
    CAP(D3D12_FEATURE_D3D12_OPTIONS16, D3D12_FEATURE_DATA_D3D12_OPTIONS16)
    CAP(D3D12_FEATURE_D3D12_TIGHT_ALIGNMENT, D3D12_FEATURE_DATA_TIGHT_ALIGNMENT)
    CAP(D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT)
#undef CAP
    if (fl.MaxSupportedFeatureLevel < D3D_FEATURE_LEVEL_12_0)
        Log("  NOTE: this GPU is below feature level 12_0. The game may still assume 12_0 features (binding tier 2+, SM 6.x, typed UAV loads...). Failures logged below with '!!' show which ones.");
}

static void OnDeviceCreated(void* pv, REFIID riid, const char* via) {
    if (!pv) return;
    IUnknown* unk = (IUnknown*)pv;
    ID3D12Device* dev = nullptr;
    if (FAILED(unk->QueryInterface(IID_ID3D12Device, (void**)&dev)) || !dev) { Log("  (created object does not expose ID3D12Device?)"); return; }
    InterlockedIncrement(&g_deviceCount);
    bool firstTimeThisDevice = (g_lastDevice != dev);
    if (firstTimeThisDevice) {
        Log("Device %p created via %s (interface %s)", (void*)dev, via, GuidName(riid));
        LogDeviceCaps(dev);
        HMODULE core = GetModuleHandleW(L"D3D12Core.dll");
        if (core) { wchar_t p[MAX_PATH]; GetModuleFileNameW(core, p, MAX_PATH); char v[64]; ModuleVersion(core, v, sizeof v); Log("D3D12 runtime in use: %s version %s", U8(p).b, v); }
        if (g_cfg.hookDevice) HookDeviceVtable(dev);
        if (g_cfg.debugLayer) {
            ID3D12InfoQueue1* iq = nullptr;
            if (SUCCEEDED(dev->QueryInterface(IID_ID3D12InfoQueue1, (void**)&iq)) && iq) {
                DWORD cookie = 0;
                HRESULT hr = iq->RegisterMessageCallback(InfoQueueCallback, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
                Log("Debug-layer message callback: %s", HrName(hr));
                iq->Release();
            } else Log("Debug-layer messages go to OutputDebugString (captured as [debug-output])");
        }
        if (g_cfg.pollDeviceRemoved) {
            ID3D12Device* prev = (ID3D12Device*)InterlockedExchangePointer((void* volatile*)&g_lastDevice, dev);
            dev = nullptr; // keep the reference for polling
            (void)prev;    // the previous device's reference is intentionally kept (it may be in use)
        } else g_lastDevice = dev;
    }
    if (dev) dev->Release();
}

static D3D_FEATURE_LEVEL OverrideFL(D3D_FEATURE_LEVEL requested, const char* where) {
    if (g_cfg.forceFL && requested > g_cfg.forceFL) {
        Log("  [override] %s: MinimumFeatureLevel %s -> %s", where, FlName(requested), FlName(g_cfg.forceFL));
        return g_cfg.forceFL;
    }
    return requested;
}

// =============================================================================
//  ID3D12DeviceFactory / ID3D12SDKConfiguration1 hooks (Agility SDK path)
// =============================================================================
typedef HRESULT (STDMETHODCALLTYPE *PFN_Factory_CreateDevice)(ID3D12DeviceFactory*, IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SdkCfg_SetSDKVersion)(ID3D12SDKConfiguration*, UINT, LPCSTR);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SdkCfg_CreateDeviceFactory)(ID3D12SDKConfiguration1*, UINT, LPCSTR, REFIID, void**);
static PFN_Factory_CreateDevice o_Factory_CreateDevice;
static PFN_SdkCfg_SetSDKVersion o_SdkCfg_SetSDKVersion;
static PFN_SdkCfg_CreateDeviceFactory o_SdkCfg_CreateDeviceFactory;

static HRESULT STDMETHODCALLTYPE H_Factory_CreateDevice(ID3D12DeviceFactory* t, IUnknown* adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void** pp) {
    bool internal = IsInternal();
    if (!internal) {
        char a[256]; LogAdapter(adapter, a, sizeof a);
        Log("ID3D12DeviceFactory::CreateDevice(adapter=%s, FeatureLevel=%s, riid=%s, ppDevice=%s)", a, FlName(fl), GuidName(riid), pp ? "ptr" : "NULL (support test)");
        PreDeviceCreate(t);
    }
    D3D_FEATURE_LEVEL use = (!internal && g_cfg.patchDeviceFactory) ? OverrideFL(fl, "ID3D12DeviceFactory::CreateDevice") : fl;
    HRESULT hr = o_Factory_CreateDevice(t, adapter, use, riid, pp);
    AddCrumb("ID3D12DeviceFactory::CreateDevice", hr);
    if (!internal) {
        Log("  -> %s", HrName(hr));
        if (SUCCEEDED(hr) && pp && *pp) OnDeviceCreated(*pp, riid, "ID3D12DeviceFactory::CreateDevice");
    }
    return hr;
}

static void HookDeviceFactory(IUnknown* obj) {
    ID3D12DeviceFactory* f = nullptr;
    if (FAILED(obj->QueryInterface(IID_ID3D12DeviceFactory, (void**)&f)) || !f) return;
    void** vt = *(void***)f;
    if (vt[IDX_DeviceFactory_CreateDevice] != (void*)&H_Factory_CreateDevice) {
        PatchSlot(vt, IDX_DeviceFactory_CreateDevice, (void*)&H_Factory_CreateDevice, (void**)&o_Factory_CreateDevice);
        Log("ID3D12DeviceFactory::CreateDevice hooked (vtable %p)", (void*)vt);
    }
    f->Release();
}

static HRESULT STDMETHODCALLTYPE H_SdkCfg_SetSDKVersion(ID3D12SDKConfiguration* t, UINT ver, LPCSTR path) {
    HRESULT hr = o_SdkCfg_SetSDKVersion(t, ver, path);
    Log("ID3D12SDKConfiguration::SetSDKVersion(%u, \"%s\") -> %s", ver, path ? path : "", HrName(hr));
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_SdkCfg_CreateDeviceFactory(ID3D12SDKConfiguration1* t, UINT ver, LPCSTR path, REFIID riid, void** pp) {
    HRESULT hr = o_SdkCfg_CreateDeviceFactory(t, ver, path, riid, pp);
    Log("ID3D12SDKConfiguration1::CreateDeviceFactory(SDKVersion=%u, path=\"%s\", %s) -> %s", ver, path ? path : "", GuidName(riid), HrName(hr));
    if (SUCCEEDED(hr) && pp && *pp && g_cfg.patchDeviceFactory) HookDeviceFactory((IUnknown*)*pp);
    return hr;
}
static void HookSdkConfig(IUnknown* obj) {
    ID3D12SDKConfiguration* c = nullptr;
    if (SUCCEEDED(obj->QueryInterface(IID_ID3D12SDKConfiguration, (void**)&c)) && c) {
        PatchSlot(*(void***)c, IDX_SDKConfiguration_SetSDKVersion, (void*)&H_SdkCfg_SetSDKVersion, (void**)&o_SdkCfg_SetSDKVersion);
        c->Release();
    }
    ID3D12SDKConfiguration1* c1 = nullptr;
    if (SUCCEEDED(obj->QueryInterface(IID_ID3D12SDKConfiguration1, (void**)&c1)) && c1) {
        PatchSlot(*(void***)c1, IDX_SDKConfiguration1_CreateDeviceFactory, (void*)&H_SdkCfg_CreateDeviceFactory, (void**)&o_SdkCfg_CreateDeviceFactory);
        c1->Release();
    }
}

// =============================================================================
//  Exported proxies
// =============================================================================
extern "C" HRESULT WINAPI Proxy_D3D12CreateDevice(IUnknown* pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid, void** ppDevice) {
    LateInit();
    InterlockedIncrement(&c_createDevice);
    char a[256]; LogAdapter(pAdapter, a, sizeof a);
    Log("D3D12CreateDevice(adapter=%s, MinimumFeatureLevel=%s, riid=%s, ppDevice=%s)", a, FlName(MinimumFeatureLevel), GuidName(riid), ppDevice ? "ptr" : "NULL (support test)");
    if (!r_CreateDevice) { Log("  -> real D3D12CreateDevice missing, returning E_FAIL"); return E_FAIL; } // original returned S_FALSE (= success!)
    PreDeviceCreate(nullptr);
    D3D_FEATURE_LEVEL use = OverrideFL(MinimumFeatureLevel, "D3D12CreateDevice");
    HRESULT hr;
    { InternalScope is; hr = r_CreateDevice(pAdapter, use, riid, ppDevice); } // internal: don't double-log the factory path
    AddCrumb("D3D12CreateDevice", hr);
    Log("  -> %s%s", HrName(hr), (hr == S_FALSE && !ppDevice) ? " (supported)" : "");
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        OnDeviceCreated(*ppDevice, riid, "D3D12CreateDevice"); ApplyGamePatches("device created"); PragRelocProbe("device created");
        if (g_cfg.agilityOverride && !g_agilityHits) Log("[agility] !! the runtime never asked through the hooked GetProcAddress - override NOT in effect (runtime reads the exe exports another way)");
    }
    return hr;
}

extern "C" HRESULT WINAPI Proxy_D3D12GetDebugInterface(REFIID riid, void** ppv) {
    LateInit();
    HRESULT hr = r_GetDebugInterface ? r_GetDebugInterface(riid, ppv) : E_NOTIMPL;
    if (!IsInternal()) Log("D3D12GetDebugInterface(%s) -> %s", GuidName(riid), HrName(hr));
    return hr;
}

extern "C" HRESULT WINAPI Proxy_D3D12GetInterface(REFCLSID clsid, REFIID riid, void** ppv) {
    LateInit();
    HRESULT hr = r_GetInterface ? r_GetInterface(clsid, riid, ppv) : E_NOTIMPL;
    if (IsInternal()) return hr;
    Log("D3D12GetInterface(%s, %s) -> %s", GuidName(clsid), GuidName(riid), HrName(hr));
    if (SUCCEEDED(hr) && ppv && *ppv) {
        if (IsEqualGUID(clsid, CLSID_D3D12DeviceFactory) && g_cfg.patchDeviceFactory) HookDeviceFactory((IUnknown*)*ppv);
        else if (IsEqualGUID(clsid, CLSID_D3D12SDKConfiguration)) HookSdkConfig((IUnknown*)*ppv);
    }
    return hr;
}

extern "C" HRESULT WINAPI Proxy_D3D12EnableExperimentalFeatures(UINT n, const IID* iids, void* cfg, UINT* sizes) {
    LateInit();
    HRESULT hr = r_EnableExperimentalFeatures ? r_EnableExperimentalFeatures(n, iids, cfg, sizes) : E_NOTIMPL;
    char list[512] = ""; size_t k = 0;
    for (UINT i = 0; iids && i < n && i < 8 && k < sizeof list; i++) k += snprintf(list + k, sizeof list - k, "%s%s", i ? ", " : "", GuidName(iids[i]));
    Log("%sD3D12EnableExperimentalFeatures(%u: %s) -> %s%s", FAILED(hr) ? "!! " : "", n, list, HrName(hr),
        FAILED(hr) ? "  (experimental features need Windows Developer Mode)" : "");
    return hr;
}

static void LogErrorBlob(ID3DBlob* e) {
    if (!e) return;
    const char* s = (const char*)e->GetBufferPointer(); size_t n = e->GetBufferSize();
    if (s && n) { char buf[1024]; size_t k = n < sizeof buf - 1 ? n : sizeof buf - 1; memcpy(buf, s, k); buf[k] = 0; Log("    error: %s", buf); }
}

extern "C" HRESULT WINAPI Proxy_D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* d, D3D_ROOT_SIGNATURE_VERSION v, ID3DBlob** blob, ID3DBlob** err) {
    ID3DBlob* myErr = nullptr;
    HRESULT hr = r_SerializeRootSignature(d, v, blob, err ? err : &myErr);
    if (FAILED(hr)) {
        Log("!! D3D12SerializeRootSignature(version 0x%x, %u params) FAILED %s", v, d ? d->NumParameters : 0, HrName(hr));
        LogErrorBlob(err ? *err : myErr);
    }
    if (myErr) myErr->Release();
    return hr;
}

extern "C" HRESULT WINAPI Proxy_D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* d, ID3DBlob** blob, ID3DBlob** err) {
    ID3DBlob* myErr = nullptr;
    HRESULT hr = r_SerializeVersionedRootSignature(d, blob, err ? err : &myErr);
    if (FAILED(hr)) {
        Log("!! D3D12SerializeVersionedRootSignature(version 0x%x) FAILED %s", d ? d->Version : 0, HrName(hr));
        LogErrorBlob(err ? *err : myErr);
    }
    if (myErr) myErr->Release();
    return hr;
}

extern "C" HRESULT WINAPI Proxy_D3D12CreateRootSignatureDeserializer(LPCVOID p, SIZE_T n, REFIID riid, void** pp) {
    HRESULT hr = r_CreateRootSignatureDeserializer(p, n, riid, pp);
    if (FAILED(hr) && !IsInternal()) Log("!! D3D12CreateRootSignatureDeserializer(%zu bytes) FAILED %s", (size_t)n, HrName(hr));
    return hr;
}
extern "C" HRESULT WINAPI Proxy_D3D12CreateVersionedRootSignatureDeserializer(LPCVOID p, SIZE_T n, REFIID riid, void** pp) {
    HRESULT hr = r_CreateVersionedRootSignatureDeserializer(p, n, riid, pp);
    if (FAILED(hr) && !IsInternal()) Log("!! D3D12CreateVersionedRootSignatureDeserializer(%zu bytes) FAILED %s", (size_t)n, HrName(hr));
    return hr;
}

// =============================================================================
//  Heartbeat thread
// =============================================================================
static DWORD WINAPI Heartbeat(void*) {
    int interval = g_cfg.heartbeatSeconds > 0 ? g_cfg.heartbeatSeconds : 5;
    for (DWORD tick = 0;; tick++) {
        Sleep(tick < 6 ? 2500 : interval * 1000);   // probe often during startup
        ID3D12Device* d = g_lastDevice;
        if (g_cfg.pollDeviceRemoved && d && !g_deviceRemovedReported) {
            HRESULT hr;
            { InternalScope is; hr = (o_GetDeviceRemovedReason ? o_GetDeviceRemovedReason(d) : d->GetDeviceRemovedReason()); }
            if (FAILED(hr)) OnDeviceRemoved(d, hr, "proxy heartbeat poll");
        }
        if (g_cfg.heartbeatSeconds <= 0) continue;
        PROCESS_MEMORY_COUNTERS_EX pm = { sizeof pm };
        K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm);
        if (g_remapFixups) Log("[alive] RemapFallback fixes so far: %ld", g_remapFixups);
        PragRelocProbe("heartbeat");
        VramLog(g_lastDevice, "heartbeat");
        Log("[alive] queue submits: direct=%ld compute=%ld copy=%ld  signals=%ld waits=%ld tileMappingUpdates=%ld RemoveDevice=%ld",
            c_exec[0], c_exec[2], c_exec[3], c_signal, c_wait, c_tile, c_removeDevice);
        Log("[alive] devices=%ld featureChecks=%ld gPSO=%ld cPSO=%ld streamPSO=%ld rootSigs=%ld resources=%ld heaps=%ld descHeaps=%ld FAILED=%ld  workingSet=%llu MB private=%llu MB",
            g_deviceCount, c_featureChecks, c_gpso, c_cpso, c_pso, c_rootsig, c_res, c_heap, c_descheap, c_fail,
            (unsigned long long)(pm.WorkingSetSize >> 20), (unsigned long long)(pm.PrivateUsage >> 20));
    }
    return 0;
}

// =============================================================================
//  DllMain
// =============================================================================
static void InitPaths() {
    GetModuleFileNameW(g_self, g_selfDir, MAX_PATH);
    *wcsrchr(g_selfDir, L'\\') = 0;
    GetModuleFileNameW(nullptr, g_exePath, MAX_PATH);
    wcscpy(g_exeDir, g_exePath);
    *wcsrchr(g_exeDir, L'\\') = 0;
    g_exeName = BaseName(g_exePath);
    swprintf(g_iniPath, MAX_PATH, L"%ls\\d3d12proxy.ini", g_selfDir);
    swprintf(g_logDir, MAX_PATH, L"%ls\\d3d12proxy_logs", g_selfDir);
    swprintf(g_dumpDir, MAX_PATH, L"%ls\\dumps", g_logDir);
    CreateDirectoryW(g_logDir, nullptr);
    CreateDirectoryW(g_dumpDir, nullptr);
}

static void OpenLog() {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t exe[64]; wcsncpy(exe, g_exeName, 63); exe[63] = 0;
    wchar_t* dot = wcsrchr(exe, L'.'); if (dot) *dot = 0;
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\d3d12proxy_%ls_%04d-%02d-%02d_%02d-%02d-%02d.log", g_logDir, exe, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    g_log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    static const char bom[] = "\xEF\xBB\xBF";
    LogWrite(bom, 3);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        g_self = hModule;
        DisableThreadLibraryCalls(hModule);
        QueryPerformanceFrequency(&g_qpcFreq); QueryPerformanceCounter(&g_qpcStart);
        InitializeCriticalSectionAndSpinCount(&g_logCs, 1000);
        g_tls = TlsAlloc();
        InitPaths();
        LoadConfig();
        OpenLog();

        SYSTEMTIME st; GetLocalTime(&st);
        Log("d3d12 proxy + logger %s (built %s %s) - %04d-%02d-%02d %02d:%02d:%02d", PROXY_VERSION, __DATE__, __TIME__, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        wchar_t self[MAX_PATH]; GetModuleFileNameW(hModule, self, MAX_PATH);
        Log("Proxy DLL: %s", U8(self).b);
        char ev[64]; ModuleVersion(GetModuleHandleW(nullptr), ev, sizeof ev);
        Log("Game: %s  version %s  pid %lu", U8(g_exePath).b, ev[0] ? ev : "?", GetCurrentProcessId());
        Log("Command line: %s", U8(GetCommandLineW()).b);
        LogSystemInfo();
        LogConfig();
        InstallExceptionHandlers();
        LogGameDirDlls();
        if (!LoadRealDll()) return FALSE;
        SetupAgilityOverride();
        CheckExports();
        CheckGameImports();
        if (g_cfg.pixAttach) PixAttach();
        ApplyGamePatches("proxy load");
        if (g_cfg.forceRelocList) { HANDLE t = CreateThread(nullptr, 0, ForceRelocListThread, nullptr, 0, nullptr); if (t) CloseHandle(t); }
        HANDLE th = CreateThread(nullptr, 0, Heartbeat, nullptr, 0, nullptr);
        if (th) CloseHandle(th);
        Log("Proxy ready, waiting for the game's D3D12 calls");
        break;
    }
    case DLL_PROCESS_DETACH:
        if (reserved) Log("Process exiting normally (ExitProcess). If the game crashed before this line, see the report above.");
        else { Log("Proxy unloaded with FreeLibrary"); if (g_real) FreeLibrary(g_real); }
        break;
    }
    return TRUE;
}
