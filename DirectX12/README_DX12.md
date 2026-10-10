# 3DMigoto DX12 (modding build) — v0.1

A `d3d12.dll` proxy that gives DirectX 12 games the parts of 3DMigoto that matter for
**modding**: shader / index-buffer / vertex-buffer hunting, frame analysis dumps that
XXMITools can import, and `[TextureOverride]`-style IB / VB replacement driven by the same
`d3dx.ini` syntax you know from the DX11 tool.

It is a native DX12 implementation (not a DX11 proxy). It does **not** wrap the whole D3D12
API; it patches the COM vtables of the objects the game creates and forwards every export to
the real system `d3d12.dll`.

> **Status — please read.** This first version was written without access to a Windows
> compiler or a DX12 game. What *was* verified, on Linux with g++:
> * the whole ini / command-list / expression / key engine (`tests/test_core.cpp`)
> * the real draw / override / shadow-copy / hashing / frame-analysis code in `Hooks12.cpp`
>   and `FrameAnalysis12.cpp`, driven against fake D3D12 objects (`tests/test_glue.cpp`)
> * that the frame-analysis file names, `log.txt` lines and vb/ib text files match the
>   regular expressions XXMITools uses to import them
> * that every `.cpp` passes a syntax check against stand-in headers
>
> What was **not** verified: compiling against the real Windows SDK, and behaviour inside real
> games. Expect to fix a few compile errors on the first build and some game-specific issues
> afterwards. `d3d12_log.txt` is written next to the dll and is the first thing to look at.

## Build

1. Visual Studio 2022 with *Desktop development with C++* (includes MSVC v143 and MASM) and
   Windows SDK 10.0.19041 or newer.
2. Copy this folder to `DirectX12\` in the 3DMigoto source tree and use the supplied
   `StereovisionHacks.sln`, which already references `DirectX12\DirectX12.vcxproj`.
3. Select **Release | x64**, right-click the `DirectX12` project → *Build*.
   Output: `builds\x64\Release\d3d12.dll`. (Only the DirectX12 project needs building.)

The project has no dependencies on the other 3DMigoto projects; it only reuses their *file
formats and ini semantics*.

## Install

Put `d3d12.dll`, `d3dx.ini` and the `Mods` / `ShaderFixes` folders next to the game's
executable (for Unreal games that is `…\Binaries\Win64`). Start the game; `d3d12_log.txt`
should contain, in this order:

```
D3D12CreateDevice -> …
Hooking ID3D12Device vtable …
DXGI factory hooks installed
Hooked command list vtable …
Swap chain: hwnd=… 
```

## Workflow

1. **Hunt** (numpad keys, same defaults as DX11 — see `d3dx.ini`). Step through index buffers
   with `Numpad7/8`; the selected mesh disappears. `Numpad9` marks it: the hash is logged,
   shown on screen and copied to the clipboard. Same for vertex buffers (`/ * -`), pixel
   shaders (`1 2 3`) and vertex shaders (`4 5 6`). `F10` reloads all inis.
2. **Dump**: press `F8`; the *next* frame is written to `FrameAnalysis-<date>-<time>\`.
3. **Import** that folder with XXMITools (Blender), edit, export.
4. **Mod ini** (hand written or from a template):

```ini
[TextureOverrideMyPosition]
hash = 1a2b3c4d                 ; vertex buffer hash from hunting / frame analysis
vb0 = ResourceMyPosition

[TextureOverrideMyIB]
hash = cafebabe                 ; index buffer hash
handling = skip                 ; hide the original mesh …
ib = ResourceMyIB
drawindexed = 12345, 0, 0       ; … and draw the replacement instead

[ResourceMyPosition]
type = Buffer
stride = 40
filename = MyPosition.buf

[ResourceMyIB]
type = Buffer
format = DXGI_FORMAT_R32_UINT
filename = MyIB.buf
```

## Hash semantics (differs from DX11 — important)

DX11 hashes a whole resource. DX12 engines usually sub-allocate many meshes in one big
buffer, so here **the hash of an IB/VB is `crc32c` of the bytes inside the bound view range**
(`BufferLocation .. +SizeInBytes`). For a mesh that has its own buffer this is the same thing.

The bytes come from a CPU-side *shadow copy* made when the game uploads data
(`CopyBufferRegion` from an upload heap, also snapshotted at `Unmap`). Buffers that are
written only by the GPU (compute / stream-out) have no CPU data: they get a size-based
placeholder hash and the log says so (`NOTE: vb… has no CPU-side data`).

Shader hashes are `fnv64` of the whole bytecode blob — the same function as DX11 3DMigoto.

## What is supported

| Area | State |
|---|---|
| `d3d12.dll` proxy, all exports forwarded | done |
| Device / resource / PSO / command list hooks (incl. `CreatePipelineState` streams, `CreateCommittedResource1/2/3`, `CreatePlacedResource`) | done |
| IB / VB hashing, shadow copies, budget (`[DX12] shadow_budget_mb`) | done |
| Hunting: ps, vs, gs, hs, ds, cs, ib, vb; mark → log + clipboard + shader `.bin` into `ShaderCache\` | done |
| Frame analysis: `log.txt`, `NNNNNN-vbN=hash-vs=…-ps=….txt/.buf`, `…-ib=….txt/.buf`, used shaders, hard-link dedupe | done |
| `[TextureOverride]` `hash`, `match_first_index/vertex/instance`, `match_index/vertex/instance_count`, `match_priority` | done (VB/IB hashes) |
| `ib =`, `vb0..vb15 =`, `handling = skip`, `draw`, `drawindexed`, `drawinstanced`, `drawindexedinstanced`, `from_caller` | done |
| `override_vertex_count`, `override_byte_stride` | done |
| `[Resource]` `type = Buffer`, `filename`, `data = "…"`, `stride`, `format`; `Resource = ref Other` | done |
| `[Constants]`, `global $var`, `$var = expr`, `if / elif / else if / else / endif`, `pre` / `post`, `run =`, `[CommandList…]`, `[Present]` | done |
| Expressions: `+ - * / // %`, comparisons, `&& \|\| !`, `time`, `hunting`, `vertex_count`, `index_count`, `first_index`, … | done |
| `[Key…]` with `type = activate / cycle / toggle / hold`, `back =`, "smart" cycling, modifiers / `NO_` keys | done |
| `[ShaderOverride]` (hash + command list), `[Include]` / `include_recursive` with per-file namespaces | done |
| Shader replacement: `ShaderFixes\<hash>-<vs|ps|gs|hs|ds|cs>_replace.bin` (compiled DXIL/DXBC) | done |
| Live reload (`reload_config`) | done |

## Not implemented yet (ignored with a `NOTE:` in the log)

* **Texture work**: `ps-t0 = …`, `this = …`, `o0 = …`, `cs-u0 = …`, texture hashes, texture
  dumps. This needs descriptor-heap / root-signature tracking and is the next big step.
  Mods that only replace meshes (IB/VB) work; the texture-swapping parts of XXMI templates
  (`run = CommandList\GIMI\SetTextures` etc.) are parsed and skipped.
* `[CustomShader]`, `[ShaderRegex]`, HLSL decompile/compile of shaders (DX11 decompiler is
  DXBC-only; DXIL needs dxc). Binary replacement files work.
* `ExecuteIndirect`, bundles (`ExecuteBundle`), mesh shaders, ray tracing, stream output.
* Draws whose geometry is generated on the GPU can be skipped / counted but not dumped.
* Multiple D3D12 devices (the first device is used for mod buffers).
* Pipeline streams that contain subobject types newer than SDK 19041 (`DEPTH_STENCIL2`,
  `RASTERIZER1/2`): parsing stops at those, shaders after them are not seen.
* Enhanced-barrier / Agility SDK features beyond `CreateCommittedResource3` creation.

## Swap chain handling (changed in v0.1.1)

v0.1 patched the shared `IDXGISwapChain` vtable. On a real game (RE4 Remake with the Steam overlay
and REFramework loaded) that produced an endless `Present -> overlay -> Present` recursion and a
stack overflow, because three pieces of software ended up hooking the same call chain. Since v0.1.1
the game receives a **proxy swap chain object** (like the DX11 3DMigoto does); the proxy calls the
real swap chain through its untouched vtable, so no other hook can call back into us. Only swap
chains created for a D3D12 command queue are proxied.

## Troubleshooting

* Game does not start / crashes: open `d3d12_log.txt`. Fatal exceptions are logged there with a
  module+offset back trace (`==== FATAL EXCEPTION …`). Then bisect with the `[DX12] hook_*`
  switches in `d3dx.ini` (turn groups off one at a time). A **Release** build is recommended;
  the Debug build is much slower and adds stack fill / JMC overhead.
* A vtable index mismatch is logged as `vtable index for … decoded as …` (harmless, the decoded
  value is used).
* Nothing hunts: look for `no CPU-side data` notes — the game uploads meshes in a way the
  shadow copy does not see; send the log.
* Overlay missing: the overlay is a separate click-through window and needs windowed or
  borderless mode. Everything is also in the log.
* Anti-cheat: injecting a graphics proxy into online games can get accounts banned.

## Files

| File | Purpose |
|---|---|
| `Proxy12.cpp`, `d3d12.def`, `d3d12_forward.asm` | exports, forwarding, device creation hook, logging |
| `Hooks12.cpp` | device / resource / PSO / command list hooks, shadow copies, hashing, draw processing, override engine |
| `Present12.cpp` | DXGI hooks, `Present`, input, hunting, overlay |
| `FrameAnalysis12.cpp` | frame analysis dumps |
| `Config12.*`, `Ini12.h`, `Dump12.h`, `Hash12.h` | portable ini / command list / format code (no Windows headers) |
| `tests/` | Linux tests (`./build_and_run_tests.sh`) |

License: this folder is meant to live in the 3DMigoto repository and should stay under the
repository's GPL license.
