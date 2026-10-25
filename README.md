# d3d12 proxy + logger (Pragmata build, TestStep 11)

Logging build of the `d3d12ProxyEdrDx11_0` wrapper, made to find out why Pragmata and
other Capcom (RE Engine) DX12 games now crash with it. It still does what the original did:

* the game loads this `d3d12.dll` instead of the real one
* `D3D12CreateDevice` is called with the minimum feature level lowered to **11_0**
* everything else goes to the real `d3d12.dll` (`d3d12_original.dll` next to the game if it
  exists, otherwise `System32\d3d12.dll`)

Same 18 exports and the same ordinals (100–117) as the 2022 release build.

## Install

1. Copy `bin\d3d12.dll` and `bin\d3d12proxy.ini` into the folder with the game's exe
   (replace the old wrapper `d3d12.dll`).
2. Start the game. A `d3d12proxy_logs` folder appears next to the exe.
3. After the crash, send `d3d12proxy_logs\d3d12proxy_<exe>_<date>.log` (and the newest
   `dumps\crash_*.dmp` if there is one).

No `d3d12proxy_logs` folder = the game never loaded the proxy (it loads d3d12 from System32
directly, or the file is in the wrong folder).

Offline only. Capcom titles have anti-tamper; don't take a modded game online.

## What the log shows

| Section | What it tells you |
|---|---|
| Header | exe + version, Windows build, CPU/RAM, ini values |
| `Real D3D12:` | which d3d12.dll was loaded and its version. If `d3d12_original.dll` is used and its version differs from System32's, it says so (a stale 2022 copy is a likely break after Windows updates) |
| `Export check` / `Game static imports of d3d12.dll` | functions the real DLL or the game has that the proxy doesn't forward |
| `Agility SDK:` | whether the exe ships its own D3D12Core.dll (`D3D12SDKVersion`/`D3D12SDKPath`) and its version |
| `GPU n:` | adapters, VRAM, driver version, and the highest D3D12 feature level each driver accepts |
| `D3D12CreateDevice(...)` / `ID3D12DeviceFactory::CreateDevice(...)` | every device creation with the requested level, `[override]` lines, and the HRESULT |
| `Device caps` | what the device really supports: max feature level, shader model, binding tier, wave ops, mesh shaders, ray tracing, enhanced barriers … |
| `CheckFeatureSupport(...)` | every feature the game queries and the answer it got (deduplicated) |
| `!! ... FAILED` | any failed PSO / root signature / resource / heap / command signature creation, with details. Shaders of failed PSOs are decoded (shader model, stage, required features from the `SFI0` flags) and saved to `dumps\*.bin` |
| `[debug-output]` | text the game printed with OutputDebugString |
| `GPU DEVICE REMOVED` | reason + DRED breadcrumbs (which command list/op the GPU died in) + page-fault info |
| `FIRST-CHANCE EXCEPTION` / `UNHANDLED EXCEPTION - GAME CRASH` | registers, faulting `module+offset`, call stack, stack scan, last D3D12 calls, module list, minidump |
| `[alive]` | heartbeat every 30 s with call counters and memory use |

A log that ends with `Process exiting normally` means the game closed itself. A log that just
stops means it was killed (TerminateProcess / fast-fail / its own crash reporter).

## v1.1 additions

* **Resource registry**: every resource created through the device is remembered (pointer →
  creating call, dimensions, format, heap). Crash reports list every `ID3D12Resource` found in
  registers / on the stack with that info and its debug name. Resources the device never created
  (swapchain buffers, other DLLs) are flagged as such.
* **Objects in registers**: other objects are shown with their vtable and MSVC RTTI class name.
* **Code snapshots**: `dumps\code_NN_<module>_rva<start>_frame<addr>.bin` hold the in-memory
  (decrypted) code around the crash address and each caller, for disassembly.
* **`[Spoof]`** ini section: override individual `CheckFeatureSupport` answers the game receives
  (feature level, shader model, binding/heap/tiled tiers, ROVs, conservative raster, mesh shaders,
  ray tracing, enhanced barriers …). Logged as `[spoof]` lines.

## Changes from the original code (besides logging)

* `D3D12CreateDevice` returned `S_FALSE` (a *success* code) with no device when the real function
  was missing; now returns `E_FAIL`.
* Forwarded exports used one shared global pointer (`FcnPtrOrg`), a race between threads; each
  export now has its own stub. Missing real exports return `E_NOTIMPL` instead of jumping to NULL.
* The real DLL is loaded by full path (DLL folder / System32) instead of `.\` relative to the
  current directory.
* The feature-level override is also applied on the `ID3D12DeviceFactory` path (`PatchDeviceFactory`).
* PIX attach is off by default (`PixAttach=1` to restore).

## Settings

See comments in `d3d12proxy.ini`. Most useful: `ForceFeatureLevel` (11_0 / 11_1 / 12_0 / 0),
`DebugLayer=1` (needs Windows "Graphics Tools"), `FirstChanceDumps=1`, `FullDump=1`.

## Building

```sh
sudo apt install g++-mingw-w64-x86-64 python3 git
cd source && ./build.sh        # -> ../bin/d3d12.dll
```

`build.sh` fetches Microsoft's DirectX-Headers (for current D3D12 interfaces), generates
`names_gen.h`, and compiles `vtbl_check.c`, which fails the build if any hooked vtable slot
doesn't match the official headers.

Source: `d3d12proxy.cpp` (everything), `d3d12proxy.def` (exports/ordinals), `vtbl_idx.h`,
`vtbl_check.c`, `gen_names.py`.

## Pragmata (RE Engine) on a GTX 960M: findings and fixes

Test machine: GTX 960M (FL 11_0, `TiledResourcesTier=1`), 4 GB VRAM. Run with `TestStep=11`.

### Fixed
1. **Tiled (reserved) textures** - `ReservedAsCommitted=1`. The game creates many render targets
   with `CreateReservedResource` and never maps tiles (`tileMappingUpdates=0`). On tiled tier 1
   an unmapped tile is undefined behaviour; the first `ClearRenderTargetView` on the 1920x1080
   R10G10B10A2 target hung the GPU (`DXGI_ERROR_DEVICE_HUNG`, DRED stopped at that clear).
   The proxy now creates non-buffer reserved resources as committed textures and ignores
   `UpdateTileMappings`/`CopyTileMappings` on them. Reserved buffers are left alone.
2. **Empty remap table** - `RemapFallback=1`. The game's remap table is empty (counter=1,
   size=0) and the lookup dereferences NULL (fault site `PRAGMATA_SKETCHBOOK.exe+0x543a2b8`).
   A vectored exception handler returns the buffer unchanged. The exe is packed, so code cannot
   be patched at runtime. This is a workaround, not a root fix; every hit costs one handled
   exception (millions per session - the log only reports the first 24 distinct buffers).

### Tried, no effect (kept as options)
`TileBacking` (map heap memory into reserved resources - hang remained), `ForceRelocation`,
`ForceRelocList`, tiled / tight-alignment / enhanced-barrier spoofs, Agility 1.619 + debug layer.

### Current state
With fixes 1+2 the game runs shader compilation to the end (~24 min, ~70k pipeline states, no
failures) and starts loading resources. It then hit `E_OUTOFMEMORY` on a 512 MB default-heap
buffer and the NVIDIA driver crashed (null write in `nvwgf2umx.dll`, D3D12Core worker thread)
about 1 s later. v1.14 adds `[vram]` logging (DXGI budget/usage) and `OomRetries`.

### Diagnostic tools
* `SyncSubmits` - fence wait after each submit to find the one that kills the device
* `RecordLists` - per command list op ring (draw, dispatch, barriers, root args, copies, clears,
  RTV/DSV clears with descriptor -> resource tracing, `OMSetRenderTargets`)
* `SkipShaders` / `SkipDraws` / `SkipDispatches` / `SkipClears` - hang bisection
* DRED breadcrumbs, shader dumps (`dumps\pso_*.bin`), `[vram]` and `[oom]` lines

### What to check first in another RE Engine title
* `TiledResourcesTier` in `CheckFeatureSupport`, many `CreateReservedResource` calls and
  `tileMappingUpdates=0` -> try `ReservedAsCommitted=1`
* NULL dereference shortly after startup in remap/relocation code -> same empty-table problem
* GPU hang -> DRED incomplete op, then `RecordLists` for the resource that op touches
* `E_OUTOFMEMORY` in `[vram]` / `!! Create*` lines -> VRAM budget problem, not a wrapper bug
