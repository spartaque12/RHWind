// RHGrass - draws GTA SA's grass (CPlantMgr, data\plants.dat) under gtaRenderHook, which has the call commented out
// (SAIdleHook.cpp, RenderForwardAfterDeferred: "//RenderGrass();"). GTA SA 1.0 US, _gtaRenderHook.asi of 2021-09-11.
//
// Three patches:
// 1. At the end of RenderForwardAfterDeferred (RH+0xEDE6E: call ebx = DefinedState; pop edi; pop esi; pop ebx) a jump
//    to a stub runs that DefinedState, then CPlantMgr::Render and DefinedState again, then the original epilogue.
// 2. Per-plant world matrix (see RenderPlant).
// 3. Plant alpha raised above RenderHook's alpha cutoff (see ScaledPlantColour).
#include <windows.h>
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

const uint32_t kSite = 0xEDE6E;                 // FF D3 5F 5E 5B: call ebx; pop edi; pop esi; pop ebx
const uint8_t kSiteBytes[] = { 0xFF, 0xD3, 0x5F, 0x5E, 0x5B };
const auto PlantMgrRender = reinterpret_cast<void(__cdecl*)()>(0x5DBAE0);
const auto DefinedState = reinterpret_cast<void(__cdecl*)()>(0x734650);

// The plant colour (plants.dat RGB x lighting, alpha = plants.dat alpha faded with distance, typically 40) is applied
// with RpGeometryForAllMaterials(geometry, 0x5DAC10, &colour) at 0x5DAED2. The grass models go through RenderHook's
// building pipeline, whose forward PS discards alpha < 0.3, so with alpha 40/255 every pixel was discarded. The
// callback pointer (push 5DAC10h at 0x5DAEC7) is replaced by one that scales the colour first.
const uint32_t kColourCallbackPush = 0x5DAEC8;   // operand of push 5DAC10h
typedef void*(__cdecl* MaterialCallback)(void* material, void* data);
const auto SetPlantColour = reinterpret_cast<MaterialCallback>(0x5DAC10);
float g_alphaScale = 6.0f, g_colourScale = 1.0f;

void* __cdecl ScaledPlantColour(void* material, void* data)
{
    uint8_t c[4];
    memcpy(c, data, 4);
    for (int i = 0; i < 3; ++i) { float v = c[i] * g_colourScale; c[i] = static_cast<uint8_t>(v > 255.0f ? 255.0f : v); }
    float a = c[3] * g_alphaScale;
    c[3] = static_cast<uint8_t>(a > 255.0f ? 255.0f : a);
    return SetPlantColour(material, c);
}

// DrawTriPlants draws every plant of a type through one atomic, moving its frame before each
// atomic->renderCallBack(atomic) (0x5DB03B: push ebx; call [ebx+48h]; add esp, 14h). RenderHook's SetMatrixBuffer only
// uploads the world matrix when the LTM pointer differs from the last one (RH+0xD0A80: g_pRenderBuffersMgr at
// RH+0x14EA64, m_pCurrentWorldMatrix +10h, m_pOldWorldMatrix +14h), and the pointer is the same for every plant, so
// they were all drawn with the first plant's matrix. The stub clears m_pOldWorldMatrix before each plant.
const uint32_t kPlantRender = 0x5DB03B;
const uint8_t kPlantRenderBytes[] = { 0x53, 0xFF, 0x53, 0x48, 0x83, 0xC4, 0x14 };
const uint32_t kRenderBuffersMgr = 0x14EA64;   // m_pOldWorldMatrix is at +14h (written as a literal below: inline asm would take a C++ constant's address)
uint32_t* g_renderBuffersMgr;

__declspec(naked) void RenderPlant()
{
    __asm {
        mov eax, g_renderBuffersMgr
        mov eax, [eax]
        test eax, eax
        jz render
        mov dword ptr [eax + 14h], 0
    render:
        push ebx
        call dword ptr [ebx + 48h]
        add esp, 4
        pop ecx             // return address (the two nops after the patched call)
        add esp, 10h        // rest of the original add esp, 14h
        jmp ecx
    }
}

bool g_enabled = true, g_keyDown;
int g_toggleKey = VK_F8;
int g_lastQuality = -1;
void* g_return;

void __cdecl RenderGrass()
{
    if (g_toggleKey) {
        bool down = (GetAsyncKeyState(g_toggleKey) & 0x8000) != 0;
        if (down && !g_keyDown) {
            g_enabled = !g_enabled;
            Log("grass %s", g_enabled ? "on" : "off");
        }
        g_keyDown = down;
    }
    if (!g_enabled) return;
    // CPlantMgr::Render draws nothing when the effects quality (g_fx.m_fxQuality, 0xA9AE00 + 0x54) is Low
    const int quality = *reinterpret_cast<volatile int*>(0xA9AE00 + 0x54);
    if (quality != g_lastQuality) {
        Log("effects quality %d%s", quality, quality == 0 ? " (Low: the game draws no grass)" : "");
        g_lastQuality = quality;
    }
    PlantMgrRender();
    DefinedState();
}

__declspec(naked) void Stub()
{
    __asm {
        call ebx            // DefinedState, as RenderHook does
        call RenderGrass
        pop edi
        pop esi
        pop ebx
        jmp g_return
    }
}

void Install()
{
    auto* rh = reinterpret_cast<uint8_t*>(GetModuleHandleA("_gtaRenderHook.asi"));
    if (!rh) { Log("_gtaRenderHook.asi not loaded, nothing patched"); return; }
    uint8_t* site = rh + kSite;
    uint8_t* plant = reinterpret_cast<uint8_t*>(kPlantRender);
    auto* push = reinterpret_cast<uint32_t*>(kColourCallbackPush);
    // all three or none: grass drawn without the other two is either invisible or piled in one spot
    if (memcmp(site, kSiteBytes, sizeof kSiteBytes) != 0) {
        Log("unsupported _gtaRenderHook.asi build (expected the 2021-09-11 one), nothing patched");
        return;
    }
    if (memcmp(plant, kPlantRenderBytes, sizeof kPlantRenderBytes) != 0 || *push != 0x5DAC10) {
        Log("game code at %08X / %08X is not the 1.0 US original (another mod patches it?), nothing patched",
            kPlantRender, kColourCallbackPush);
        return;
    }

    DWORD prot;
    g_renderBuffersMgr = reinterpret_cast<uint32_t*>(rh + kRenderBuffersMgr);
    VirtualProtect(plant, sizeof kPlantRenderBytes, PAGE_EXECUTE_READWRITE, &prot);
    plant[0] = 0xE8;
    *reinterpret_cast<int32_t*>(plant + 1) = static_cast<int32_t>(reinterpret_cast<uint8_t*>(&RenderPlant) - (plant + 5));
    plant[5] = plant[6] = 0x90;
    VirtualProtect(plant, sizeof kPlantRenderBytes, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), plant, sizeof kPlantRenderBytes);

    VirtualProtect(push, 4, PAGE_EXECUTE_READWRITE, &prot);
    *push = reinterpret_cast<uint32_t>(&ScaledPlantColour);
    VirtualProtect(push, 4, prot, &prot);

    g_return = site + 5;
    VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &prot);
    site[0] = 0xE9;
    *reinterpret_cast<int32_t*>(site + 1) = static_cast<int32_t>(reinterpret_cast<uint8_t*>(&Stub) - (site + 5));
    VirtualProtect(site, 5, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), site, 5);

    Log("grass pass added after RenderHook's forward pass; %s, toggle key 0x%X, alpha x%.2f, colour x%.2f",
        g_enabled ? "on" : "off", g_toggleKey, g_alphaScale, g_colourScale);
}

}

BOOL WINAPI DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        char path[MAX_PATH];
        GetModuleFileNameA(mod, path, MAX_PATH);
        if (char* dot = strrchr(path, '.')) strcpy_s(dot, path + MAX_PATH - dot, ".log");
        g_log = _fsopen(path, "w", _SH_DENYNO);
        Log("RHGrass 1.0");
        if (char* dot = strrchr(path, '.')) strcpy_s(dot, path + MAX_PATH - dot, ".ini");
        g_enabled = GetPrivateProfileIntA("Grass", "Enabled", 1, path) != 0;
        char key[32];
        GetPrivateProfileStringA("Grass", "ToggleKey", "0x77", key, sizeof key, path);
        g_toggleKey = static_cast<int>(strtol(key, nullptr, 0));
        char num[32];
        GetPrivateProfileStringA("Grass", "AlphaScale", "6", num, sizeof num, path);
        g_alphaScale = static_cast<float>(atof(num));
        GetPrivateProfileStringA("Grass", "ColourScale", "1", num, sizeof num, path);
        g_colourScale = static_cast<float>(atof(num));
        Install();
    }
    return TRUE;
}
