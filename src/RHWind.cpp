// RHWind - vegetation wind for gtaRenderHook, compatible with models made for SkyGfx Extended's wind shader
// (e.g. "BSOR 5 Stars - Wind Shader Adapted"). GTA SA 1.0 US, _gtaRenderHook.asi build of 2021-09-11.
//
// SkyGfx Extended marks the meshes that sway with RenderWare atomic plugin 0xEDED (low byte 1 = wind) and uses
// 1 - (day prelight alpha) as the per-vertex amount. RenderHook keeps the day prelight colour in its building
// vertices, so only three things are missing, and this plugin adds them:
//  - the 0xEDED atomic plugin, registered in the game's PluginAttach (call at 0x53D903) so the flag is kept;
//  - per draw, the wind values in a constant buffer (VS b12): RenderHook's building render callbacks are wrapped;
//  - the displacement itself, in shaders\Wind.hlsli, used by SACustomBuilding.hlsl / SACustomBuildingDN.hlsl.
// The values follow SkyGfx Extended: windPos = object position + a scroll that moves with CWeather::WindDir,
// windIntensity = CWeather::Wind * 1.5 + 2.
//
// Dual-pass foliage ([DualPass] in RHWind.ini): RenderHook either sends every mesh with alpha to its forward
// alpha-blended list (UseDitheringForAlphaObjects="false": leaves get no sun shadows and look washed out) or draws
// it dithered into the G-buffer ("true": noisy). With dual pass, a building DN mesh whose transparency comes from its
// texture (material alpha 255; vertex alpha only on wind meshes, where it is the wind weight) is drawn both ways:
// alpha-tested at Cutoff into the G-buffer (lit and shadowed like opaque geometry), and in the forward pass only the
// pixels below Cutoff are blended, for soft edges. The AlphaMesh entry's unused padding carries the flags
// (bit 0 dual pass, bit 1 wind) to the forward pass, which now also gets each mesh's own wind values.
//
// Distant dither ([DualPass] DitherStart/DitherEnd): far away, alpha-tested plus blended leaves shimmer. There the
// G-buffer pass keeps each pixel with probability = alpha (screen-door) and the forward pass draws nothing; the
// pattern changes every frame (frame number in b12) so TAA turns it into smooth coverage.
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <share.h>

namespace {

FILE* g_log;

void Log(const char* fmt, ...)
{
    if (!g_log) return;
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

// ------------------------------------------------------------------------------------------------ game / RenderWare
const uintptr_t kPluginAttachCall = 0x53D903;   // call inside PluginAttach; SkyGfx hooks the same place
const uint32_t kWindPluginId = 0xEDED;

typedef int(__cdecl* RegisterPluginFn)(int size, uint32_t id, void* ctor, void* dtor, void* copy);
typedef int(__cdecl* RegisterPluginStreamFn)(uint32_t id, void* read, void* write, void* getSize);
typedef uint32_t(__cdecl* StreamReadFn)(void* stream, void* buffer, uint32_t length);
typedef uint8_t*(__cdecl* FrameGetLTMFn)(void* frame);
typedef void*(__cdecl* StreamSkipFn)(void* stream, uint32_t length);
const auto RpAtomicRegisterPlugin = reinterpret_cast<RegisterPluginFn>(0x74BDA0);
const auto RpAtomicRegisterPluginStream = reinterpret_cast<RegisterPluginStreamFn>(0x74BE00);
const auto RwStreamRead = reinterpret_cast<StreamReadFn>(0x7EC9D0);
const auto RwStreamSkip = reinterpret_cast<StreamSkipFn>(0x7ECD00);
const auto RwFrameGetLTM = reinterpret_cast<FrameGetLTMFn>(0x7F0990);

inline float CWeatherWind() { return *reinterpret_cast<volatile float*>(0xC812F0); }
inline const float* CWeatherWindDir() { return reinterpret_cast<const float*>(0xC813E0); }
inline float TimeStep() { return *reinterpret_cast<volatile float*>(0xB7CB5C); }
inline uint32_t FrameCounter() { return *reinterpret_cast<volatile uint32_t*>(0xB7CB4C); }

int g_pluginOffset = -1;
uintptr_t g_origPluginAttach;

void* __cdecl WindCtor(void* object, int offset, int) { *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(object) + offset) = 0; return object; }
void* __cdecl WindCopy(void* dst, const void* src, int offset, int)
{
    *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(dst) + offset) = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(src) + offset);
    return dst;
}
void* __cdecl WindRead(void* stream, int length, void* object, int offset, int)
{
    const uint32_t n = length < 4 ? length : 4;
    if (n && RwStreamRead(stream, static_cast<uint8_t*>(object) + offset, n) != n) return nullptr;
    if (length > 4) RwStreamSkip(stream, length - 4);
    return stream;
}
int __cdecl WindSize(const void*, int, int) { return 0; }   // never written back

bool HasWind(void* atomic) { return g_pluginOffset >= 0 && (*(static_cast<uint8_t*>(atomic) + g_pluginOffset) == 1); }

// ------------------------------------------------------------------------------------------------ D3D11
ID3D11Device* g_device;
ID3D11DeviceContext* g_context;
ID3D11Buffer* g_windBuffer;
float g_lastWind[12] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };

bool g_dualPass = true;
float g_cutoff = 0.5f;
float g_fadeStart = 150.0f, g_fadeEnd = 350.0f;   // view depth; FadeEnd 0 = no distance fade
float g_mipBoost = 0.25f;
float g_ditherStart = 150.0f, g_ditherEnd = 200.0f;   // view depth; DitherEnd <= DitherStart = no distant dither

typedef HRESULT(WINAPI* CreateDeviceFn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                         ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
CreateDeviceFn g_origCreateDevice;

HRESULT WINAPI CreateDeviceHook(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags, const D3D_FEATURE_LEVEL* levels,
                                UINT numLevels, UINT sdk, ID3D11Device** device, D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context)
{
    const HRESULT hr = g_origCreateDevice(adapter, type, sw, flags, levels, numLevels, sdk, device, level, context);
    if (SUCCEEDED(hr) && device && *device && context && *context && !g_device) {
        g_device = *device;
        g_context = *context;
        Log("captured RenderHook's D3D11 device %p", g_device);
    }
    return hr;
}

bool EnsureBuffer()
{
    if (g_windBuffer) return true;
    if (!g_device) return false;
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = 48;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_device->CreateBuffer(&desc, nullptr, &g_windBuffer))) { g_device = nullptr; Log("wind constant buffer: CreateBuffer failed"); return false; }
    return true;
}

// ------------------------------------------------------------------------------------------------ per draw
typedef void(__cdecl* RenderCB)(void* repEntry, void* object, uint8_t type, uint32_t flags);
RenderCB g_rhBuildingRender, g_rhBuildingDNRender;
float g_scroll[2];
uint32_t g_scrollFrame;

// cbuffer b12 (VS and PS): vWind = { pos.xy + scroll, intensity, has wind },
// vAlphaPass = { forward pass of a dual-pass mesh, cutoff (0 = dual pass off), fade start, fade end },
// vAlphaParams = { alpha boost per mip, dither start, dither end, frame number (dither pattern) }.
void SetWindBuffer(bool wind, const float* pos, bool fringePass)
{
    if (!EnsureBuffer()) return;
    const uint32_t frame = FrameCounter();
    if (frame != g_scrollFrame) {   // once per frame: the gusts travel along the wind direction
        g_scrollFrame = frame;
        const float* dir = CWeatherWindDir();
        g_scroll[0] += (dir[0] * 0.01f + 0.01f) * TimeStep();
        g_scroll[1] += (dir[1] * 0.01f + 0.01f) * TimeStep();
    }
    float values[12] = { 0, 0, 0, 0, fringePass ? 1.0f : 0.0f, g_dualPass ? g_cutoff : 0.0f, g_fadeStart, g_fadeEnd,
                         g_mipBoost, g_ditherStart, g_ditherEnd, static_cast<float>(frame & 63) };
    if (wind) {
        values[0] = (pos ? pos[0] : 0.0f) + g_scroll[0];
        values[1] = (pos ? pos[1] : 0.0f) + g_scroll[1];
        values[2] = CWeatherWind() * 1.5f + 2.0f;
        values[3] = 1.0f;
    }
    if (memcmp(values, g_lastWind, sizeof values) != 0) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(g_context->Map(g_windBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            memcpy(mapped.pData, values, sizeof values);
            g_context->Unmap(g_windBuffer, 0);
            memcpy(g_lastWind, values, sizeof values);
        }
    }
    g_context->VSSetConstantBuffers(12, 1, &g_windBuffer);
    g_context->PSSetConstantBuffers(12, 1, &g_windBuffer);
}

void SetWindFor(void* atomic)
{
    const float* pos = nullptr;
    const bool wind = HasWind(atomic);
    if (wind) {
        void* rwFrame = *reinterpret_cast<void**>(static_cast<uint8_t*>(atomic) + 4);   // RwObject::parent
        const uint8_t* ltm = rwFrame ? RwFrameGetLTM(rwFrame) : nullptr;
        pos = ltm ? reinterpret_cast<const float*>(ltm + 0x30) : nullptr;
    }
    SetWindBuffer(wind, pos, false);
}

// ------------------------------------------------------------------------------------------------ dual pass
struct AlphaMesh { void* entry; const uint8_t* worldMatrix; int meshId; uint32_t flags; };   // RenderHook's; padding = flags
const uint32_t kFlagDualPass = 1, kFlagWind = 2;

// RenderHook+0xCA304, in CCustomBuildingDNPipeline::Render's G-buffer stage, builds the AlphaMesh at [ebp-0x90]
// (padding [ebp-0x84] = 0), pushes it to m_aAlphaMeshList and skips the draw. The stub fills the padding, pushes,
// then sends dual-pass meshes on to the G-buffer draw (+0xCA319, the path UseDitheringForAlphaObjects="true" takes).
uintptr_t g_poolPush, g_drawPath, g_nextMesh, g_origDoCall;

uint32_t __cdecl AlphaMeshFlags(const uint8_t* material, uint32_t vertexAlpha, void* atomic)
{
    const bool wind = HasWind(atomic);
    uint32_t flags = wind ? kFlagWind : 0;
    if (g_dualPass && material && material[7] == 255 && (!vertexAlpha || wind))   // RpMaterial::color.alpha
        flags |= kFlagDualPass;
    return flags;
}

__declspec(naked) void PushStub()
{
    __asm {
        push eax                        // &AlphaMesh
        push ecx                        // m_aAlphaMeshList
        push dword ptr [ebp - 0x7C]     // atomic
        push dword ptr [ebp - 0xA8]     // mesh.vertexAlpha
        push dword ptr [ebp - 0xAC]     // mesh.material
        call AlphaMeshFlags
        add esp, 12
        mov dword ptr [ebp - 0x84], eax
        mov edx, eax
        pop ecx
        pop eax
        push edx
        push eax
        call dword ptr [g_poolPush]     // CRenderMeshPool<AlphaMesh>::Push, thiscall
        pop edx
        test dl, 1
        jz next
        jmp dword ptr [g_drawPath]
    next:
        jmp dword ptr [g_nextMesh]
    }
}

// Forward pass: RenderAlphaList's lambda, called through its std::function's _Do_call (thiscall, arg = AlphaMesh&).
void __cdecl SetAlphaPassFor(const AlphaMesh* mesh)
{
    const bool wind = (mesh->flags & kFlagWind) != 0;
    SetWindBuffer(wind, wind && mesh->worldMatrix ? reinterpret_cast<const float*>(mesh->worldMatrix + 0x30) : nullptr,
                  (mesh->flags & kFlagDualPass) != 0);
}

__declspec(naked) void DoCallHook()
{
    __asm {
        push ecx
        push dword ptr [esp + 8]
        call SetAlphaPassFor
        add esp, 4
        pop ecx
        jmp dword ptr [g_origDoCall]
    }
}

void __cdecl BuildingRenderHook(void* repEntry, void* object, uint8_t type, uint32_t flags)
{
    SetWindFor(object);
    g_rhBuildingRender(repEntry, object, type, flags);
}

void __cdecl BuildingDNRenderHook(void* repEntry, void* object, uint8_t type, uint32_t flags)
{
    SetWindFor(object);
    g_rhBuildingDNRender(repEntry, object, type, flags);
}

// ------------------------------------------------------------------------------------------------ install
void WritePointer(uintptr_t at, const void* value)
{
    DWORD prot;
    VirtualProtect(reinterpret_cast<void*>(at), 4, PAGE_EXECUTE_READWRITE, &prot);
    *reinterpret_cast<const void**>(at) = value;
    VirtualProtect(reinterpret_cast<void*>(at), 4, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), 4);
}

void WriteCode(uint8_t* at, const uint8_t* bytes, size_t size)
{
    DWORD prot;
    VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &prot);
    memcpy(at, bytes, size);
    VirtualProtect(at, size, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), at, size);
}

void InstallDualPass(uint8_t* rh)
{
    // mov dword ptr [ebp-0x84],0 / push eax / call Push / jmp next mesh
    static const uint8_t kPushSite[] = { 0xC7, 0x85, 0x7C, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x50,
                                         0xE8, 0x2C, 0xD4, 0xFF, 0xFF, 0xE9, 0xFD, 0x00, 0x00, 0x00 };
    uint8_t* site = rh + 0xCA304;
    auto* doCall = reinterpret_cast<uintptr_t*>(rh + 0x133D6C);   // std::function vtable of the DN RenderAlphaList lambda
    if (memcmp(site, kPushSite, sizeof kPushSite) != 0 || *doCall != reinterpret_cast<uintptr_t>(rh + 0xCAB70)) {
        Log("dual pass: RenderHook code differs from the 2021-09-11 build - not installed");
        g_dualPass = false;
        return;
    }
    g_poolPush = reinterpret_cast<uintptr_t>(rh + 0xC7740);
    g_drawPath = reinterpret_cast<uintptr_t>(rh + 0xCA319);
    g_nextMesh = reinterpret_cast<uintptr_t>(rh + 0xCA416);
    g_origDoCall = *doCall;
    WritePointer(reinterpret_cast<uintptr_t>(doCall), reinterpret_cast<void*>(&DoCallHook));
    uint8_t jmp[5] = { 0xE9 };
    *reinterpret_cast<int32_t*>(jmp + 1) = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&PushStub) - reinterpret_cast<uintptr_t>(site + 5));
    WriteCode(site, jmp, sizeof jmp);
    Log("dual pass: hooks installed (%s, cutoff %.2f, fade %.0f-%.0f, mip boost %.2f, dither %.0f-%.0f)", g_dualPass ? "on" : "off", g_cutoff,
        g_fadeStart, g_fadeEnd, g_mipBoost, g_ditherStart, g_ditherEnd);
}

bool InRange(uintptr_t p, uint8_t* lo, size_t size) { return p >= reinterpret_cast<uintptr_t>(lo) && p < reinterpret_cast<uintptr_t>(lo) + size; }

// Replaces RenderHook's pointer at each site (they must all hold the same RenderHook function) with the wrapper.
bool WrapRenderSites(const uintptr_t* sites, int count, RenderCB& original, RenderCB wrapper, uint8_t* rh, size_t rhSize, const char* name)
{
    const uintptr_t first = *reinterpret_cast<uintptr_t*>(sites[0]);
    for (int i = 0; i < count; ++i)
        if (*reinterpret_cast<uintptr_t*>(sites[i]) != first || !InRange(first, rh, rhSize)) {
            Log("%s: site 0x%06X does not hold RenderHook's callback - not wrapped", name, unsigned(sites[i]));
            return false;
        }
    original = reinterpret_cast<RenderCB>(first);
    for (int i = 0; i < count; ++i) WritePointer(sites[i], reinterpret_cast<void*>(wrapper));
    Log("%s: RenderHook callback %p wrapped at %d site(s)", name, reinterpret_cast<void*>(first), count);
    return true;
}

void InstallRenderHookParts()
{
    auto* rh = reinterpret_cast<uint8_t*>(GetModuleHandleA("_gtaRenderHook.asi"));
    if (!rh) { Log("_gtaRenderHook.asi not loaded - nothing to do"); return; }
    const auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(rh + reinterpret_cast<IMAGE_DOS_HEADER*>(rh)->e_lfanew);
    const size_t rhSize = nt->OptionalHeader.SizeOfImage;

    // D3D11CreateDevice import of _gtaRenderHook.asi (2021-09-11 build)
    auto** slot = reinterpret_cast<CreateDeviceFn*>(rh + 0x1251C0);
    const auto real = reinterpret_cast<CreateDeviceFn>(GetProcAddress(GetModuleHandleA("d3d11.dll"), "D3D11CreateDevice"));
    if (!real || *slot != real) { Log("RenderHook's D3D11CreateDevice import not found - wind disabled"); return; }
    g_origCreateDevice = *slot;
    WritePointer(reinterpret_cast<uintptr_t>(slot), reinterpret_cast<void*>(&CreateDeviceHook));

    static const uintptr_t kBuildingSites[] = { 0x5D7B0B, 0x7578AE };              // CCustomBuildingPipeline::Patch
    static const uintptr_t kBuildingDNSites[] = { 0x5D67F4, 0x815EDF, 0x815F65 };  // CCustomBuildingDNPipeline::Patch
    WrapRenderSites(kBuildingSites, 2, g_rhBuildingRender, &BuildingRenderHook, rh, rhSize, "building pipeline");
    WrapRenderSites(kBuildingDNSites, 3, g_rhBuildingDNRender, &BuildingDNRenderHook, rh, rhSize, "building DN pipeline");
    InstallDualPass(rh);   // also when [DualPass] is off: its forward-pass hook gives alpha-listed wind meshes their wind values
}

int __cdecl PluginAttachHook()
{
    const int result = reinterpret_cast<int(__cdecl*)()>(g_origPluginAttach)();
    g_pluginOffset = RpAtomicRegisterPlugin(4, kWindPluginId, reinterpret_cast<void*>(&WindCtor), nullptr, reinterpret_cast<void*>(&WindCopy));
    if (g_pluginOffset < 0 || RpAtomicRegisterPluginStream(kWindPluginId, reinterpret_cast<void*>(&WindRead), nullptr,
                                                            reinterpret_cast<void*>(&WindSize)) < 0) {
        Log("atomic plugin 0x%X: registration failed (already registered by SkyGfx?) - wind disabled", kWindPluginId);
        g_pluginOffset = -1;
        return result;
    }
    Log("atomic plugin 0x%X registered at offset %d", kWindPluginId, g_pluginOffset);
    InstallRenderHookParts();
    return result;
}

void Install()
{
    auto* call = reinterpret_cast<uint8_t*>(kPluginAttachCall);
    if (call[0] != 0xE8) { Log("0x%06X is not a call - not installed", unsigned(kPluginAttachCall)); return; }
    g_origPluginAttach = kPluginAttachCall + 5 + *reinterpret_cast<int32_t*>(call + 1);
    DWORD prot;
    VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &prot);
    *reinterpret_cast<int32_t*>(call + 1) = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&PluginAttachHook) - (kPluginAttachCall + 5));
    VirtualProtect(call + 1, 4, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    Log("hooked the PluginAttach call at 0x%06X (previous target 0x%06X)", unsigned(kPluginAttachCall), unsigned(g_origPluginAttach));
}

} // namespace

BOOL WINAPI DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        char path[MAX_PATH];
        GetModuleFileNameA(mod, path, MAX_PATH);
        if (char* dot = strrchr(path, '.')) strcpy_s(dot, path + MAX_PATH - dot, ".log");
        g_log = _fsopen(path, "w", _SH_DENYNO);
        Log("RHWind 1.2");
        if (char* dot = strrchr(path, '.')) strcpy_s(dot, path + MAX_PATH - dot, ".ini");
        g_dualPass = GetPrivateProfileIntA("DualPass", "Enabled", 1, path) != 0;
        char cutoff[32];
        GetPrivateProfileStringA("DualPass", "Cutoff", "0.5", cutoff, sizeof cutoff, path);
        g_cutoff = static_cast<float>(atof(cutoff));
        if (g_cutoff < 0.05f) g_cutoff = 0.05f;
        if (g_cutoff > 0.99f) g_cutoff = 0.99f;
        GetPrivateProfileStringA("DualPass", "FadeStart", "150", cutoff, sizeof cutoff, path);
        g_fadeStart = static_cast<float>(atof(cutoff));
        GetPrivateProfileStringA("DualPass", "FadeEnd", "350", cutoff, sizeof cutoff, path);
        g_fadeEnd = static_cast<float>(atof(cutoff));
        GetPrivateProfileStringA("DualPass", "MipAlphaBoost", "0.25", cutoff, sizeof cutoff, path);
        g_mipBoost = static_cast<float>(atof(cutoff));
        GetPrivateProfileStringA("DualPass", "DitherStart", "150", cutoff, sizeof cutoff, path);
        g_ditherStart = static_cast<float>(atof(cutoff));
        GetPrivateProfileStringA("DualPass", "DitherEnd", "200", cutoff, sizeof cutoff, path);
        g_ditherEnd = static_cast<float>(atof(cutoff));
        Install();
    }
    return TRUE;
}
