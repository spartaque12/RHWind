// SAProcObjLimit - raises GTA SA's limit of 128 procedural object types (data\procobj.dat lines). GTA SA 1.0 US.
//
// CProcObjectMan (g_procObjMan, 0xBB7CB0) is
//   +0x0000 int  numAllocatedMatrices
//   +0x0004 int  numSurfaceInfos
//   +0x0008 ProcSurfaceInfo infos[128]      (0x48 each)
//   +0x2408 ProcObjectListItem pool[512]    (0x14 each)
//   +0x4C08 TList freeList                  (0x0C)
// The loader (0x5A3140) adds one info per line with no bound check, so a procobj.dat with more than 128 lines (easy
// with modloader merging several mods' files) overwrites the pool, and the game later crashes walking a corrupted
// object list (e.g. at 0x5A3FA6 in ProcessTriangleRemoved). This plugin moves the whole object to a buffer with room
// for kMaxInfos infos and rewrites every reference: the object address, the pool/free-list offsets and the info count
// in its constructor/destructor. All constructors only zero memory, so the zeroed buffer is already "constructed".
#include <windows.h>
#include <cstdio>
#include <cstdint>
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

const uint32_t kOldObject = 0xBB7CB0;
const uint32_t kInfoSize = 0x48, kOldInfos = 128;
const uint32_t kMaxInfos = 512;
const uint32_t kOldPool = 0x2408, kOldFreeList = 0x4C08, kObjectSize = 0x4C14;
const uint32_t kDelta = (kMaxInfos - kOldInfos) * kInfoSize;

alignas(16) uint8_t g_object[kObjectSize + kDelta];

struct Patch {
    uint32_t at;         // address of the 4-byte operand
    uint32_t expected;   // its original value
    uint32_t value;      // what to write
};

bool Apply(const Patch* patches, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        uint32_t cur = *reinterpret_cast<const uint32_t*>(patches[i].at);
        if (cur != patches[i].expected) {
            Log("unexpected value %08X at %08X (expected %08X): another mod patches this, or not the 1.0 US exe. Nothing changed.",
                cur, patches[i].at, patches[i].expected);
            return false;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        DWORD prot;
        void* p = reinterpret_cast<void*>(patches[i].at);
        VirtualProtect(p, 4, PAGE_EXECUTE_READWRITE, &prot);
        *static_cast<uint32_t*>(p) = patches[i].value;
        VirtualProtect(p, 4, prot, &prot);
    }
    return true;
}

// CProcObjectMan::Init (loads procobj.dat), wrapped to log how many types were loaded
const uint32_t kInit = 0x5A3EA0;

bool __fastcall InitHook(uint8_t* self)
{
    bool ok = reinterpret_cast<bool(__thiscall*)(void*)>(kInit)(self);
    const uint32_t count = *reinterpret_cast<uint32_t*>(self + 4);
    Log("procobj.dat loaded: %u types%s", count, count > kMaxInfos ? " - MORE THAN THE LIMIT, memory corrupted" : "");
    return ok;
}

void RedirectCall(uint32_t at)
{
    uint8_t* p = reinterpret_cast<uint8_t*>(at);
    if (p[0] != 0xE8 || reinterpret_cast<uint32_t>(p + 5) + *reinterpret_cast<int32_t*>(p + 1) != kInit) {
        Log("call to CProcObjectMan::Init not found at %08X (load count not logged)", at);
        return;
    }
    DWORD prot;
    VirtualProtect(p + 1, 4, PAGE_EXECUTE_READWRITE, &prot);
    *reinterpret_cast<int32_t*>(p + 1) = static_cast<int32_t>(reinterpret_cast<uint8_t*>(&InitHook) - (p + 5));
    VirtualProtect(p + 1, 4, prot, &prot);
}

void Install()
{
    const uint32_t obj = reinterpret_cast<uint32_t>(g_object);
    const uint32_t pool = kOldPool + kDelta, freeList = kOldFreeList + kDelta;
    const Patch patches[] = {
        // mov ecx, offset g_procObjMan
        { 0x53BE4F, kOldObject, obj },      // CGame::Init -> Init
        { 0x53C211, kOldObject, obj },      // CGame::Process -> (empty) Update
        { 0x53C5B3, kOldObject, obj },      // CGame::Shutdown -> Shutdown
        { 0x53C917, kOldObject, obj },      // CGame::ShutDownForRestart -> Shutdown
        { 0x5BA464, kOldObject, obj },      // re-init -> Init
        { 0x5DB6E4, kOldObject, obj },      // CPlantLocTri::Release -> ProcessTriangleRemoved
        { 0x5DBF67, kOldObject, obj },      // CPlantMgr -> ProcessTriangleAdded
        { 0x5DCD2D, kOldObject, obj },      // CPlantMgr -> ProcessTriangleAdded
        { 0x84FDC1, kOldObject, obj },      // static constructor
        { 0x856671, kOldObject, obj },      // atexit destructor
        // numAllocatedMatrices, by absolute address
        { 0x5A3322, kOldObject, obj },      // cmp [g_procObjMan], 200
        { 0x5A36D4, kOldObject, obj },      // inc [g_procObjMan]
        // free list, by absolute address
        { 0x5A328D, kOldObject + kOldFreeList, obj + freeList },
        { 0x5A32F2, kOldObject + kOldFreeList, obj + freeList },
        { 0x5A3FCD, kOldObject + kOldFreeList, obj + freeList },
        // this-relative pool / free list offsets
        { 0x5A30AF, kOldFreeList, freeList },   // destructor: lea ecx, [esi+4C08h]
        { 0x5A30CE, kOldPool, pool },           // destructor: lea eax, [esi+2408h]
        { 0x5A3122, kOldFreeList, freeList },   // add ecx, 4C08h
        { 0x5A3132, kOldFreeList, freeList },   // add ecx, 4C08h
        { 0x5A3E6A, kOldPool, pool },           // constructor: lea ecx, [esi+2408h]
        { 0x5A3E7E, kOldFreeList, freeList },   // constructor: lea ecx, [esi+4C08h]
        { 0x5A3EA8, kOldFreeList, freeList },   // Init: lea ebp, [edi+4C08h]
        { 0x5A3EAE, kOldPool, pool },           // Init: lea esi, [edi+2408h]
        { 0x5A3F07, kOldFreeList, freeList },   // Shutdown: lea ecx, [ebx+4C08h]
        // number of infos for the constructor / destructor array iterators (push 80h)
        { 0x5A3E44, kOldInfos, kMaxInfos },
        { 0x5A30E3, kOldInfos, kMaxInfos },
    };
    if (!Apply(patches, sizeof patches / sizeof patches[0])) return;
    Log("procedural object types: limit %u -> %u (object moved to %08X)", kOldInfos, kMaxInfos, obj);
    RedirectCall(0x53BE53);
    RedirectCall(0x5BA468);
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
        Log("SAProcObjLimit 1.0");
        Install();
    }
    return TRUE;
}
