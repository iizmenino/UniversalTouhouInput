#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dinput.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// This plugin intentionally does not link against thcrap.lib.
// It resolves the small thcrap API surface it needs at runtime, just like
// TouhouKeymap does. That makes the DLL easier to build and distribute.

namespace UTI {

static int (*detour_chain)(const char*, int, ...)=nullptr;
static const char* (*runconfig_game_get)()=nullptr;

// Win32 keyboard API chains.
static BOOL (WINAPI *chain_GetKeyboardState)(PBYTE) = ::GetKeyboardState;
static SHORT (WINAPI *chain_GetAsyncKeyState)(int) = ::GetAsyncKeyState;
static SHORT (WINAPI *chain_GetKeyState)(int) = ::GetKeyState;

// DirectInput chains.
static HRESULT (WINAPI *chain_DirectInput8Create)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN) = ::DirectInput8Create;
static HRESULT (WINAPI *chain_CreateDevice)(IDirectInput8A*, REFGUID, LPDIRECTINPUTDEVICEA*, LPUNKNOWN) = nullptr;
static HRESULT (WINAPI *chain_GetDeviceState)(IDirectInputDevice8A*, DWORD, LPVOID) = nullptr;
static HRESULT (WINAPI *chain_SetDataFormat)(IDirectInputDevice8A*, LPCDIDATAFORMAT) = nullptr;

// scan code -> DirectInput object index. 0xFFFF means unmapped.
static uint16_t g_scan_to_di[512];
static bool g_di_map_ready = false;

template <typename T>
static void UpdateVTable(void*& entry, T replacement, T& oldFunc)
{
    if ((uintptr_t)entry != (uintptr_t)replacement) {
        oldFunc = (T)entry;
        entry = (void*)(uintptr_t)replacement;
    }
}

static bool is_supported_game(const char* game)
{
    if (!game || game[0] != 't' || game[1] != 'h') {
        return false;
    }

    // Accept th06..th20 and their thcrap variants such as th06_custom.
    // The numeric part is deliberately parsed instead of hard-coding every
    // spelling, so *_custom and future launcher suffixes remain compatible.
    char* end = nullptr;
    long number = strtol(game + 2, &end, 10);
    if (end == game + 2) {
        return false;
    }

    return number >= 6 && number <= 20;
}

static unsigned normalize_dinput_scan_code(DWORD scan)
{
    // Matches the convention used by TouhouKeymap: KF_EXTENDED lives in the
    // upper half of our 9-bit map.
    UINT code = (BYTE)scan;
    if (code == 0xE0) {
        return (BYTE)(scan >> 8) | KF_EXTENDED;
    }
    if (code == 0xE1) {
        return (BYTE)(scan >> 16) | KF_EXTENDED;
    }
    return code;
}

static unsigned normalize_vk_scan_code(UINT vk)
{
    HKL layout = GetKeyboardLayout(0);
    UINT scan = MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC_EX, layout);
    if (!scan) {
        scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    }
    if (!scan) {
        return 0;
    }

    const BYTE high = (BYTE)(scan >> 8);
    if (high == 0xE0 || high == 0xE1) {
        return (scan & 0xFF) | KF_EXTENDED;
    }
    return scan & 0xFF;
}

static int di_index_for_vk(UINT vk)
{
    if (!g_di_map_ready) {
        return -1;
    }

    const unsigned scan = normalize_vk_scan_code(vk);
    if (scan >= sizeof(g_scan_to_di) / sizeof(g_scan_to_di[0])) {
        return -1;
    }

    const uint16_t index = g_scan_to_di[scan];
    return index == 0xFFFF ? -1 : (int)index;
}

static bool read_unified_win32_state(BYTE out[256])
{
    if (!chain_GetKeyboardState || !chain_GetAsyncKeyState) {
        return false;
    }

    if (!chain_GetKeyboardState(out)) {
        return false;
    }

    // GetKeyboardState is the normal path. OR in the current async state as a
    // second source so SendInput-style/injected state that reaches only one of
    // the Win32 state layers still reaches the game.
    for (int vk = 0; vk < 256; ++vk) {
        if (chain_GetAsyncKeyState(vk) & 0x8000) {
            out[vk] |= 0x80;
        }
    }

    return true;
}

// -----------------------------------------------------------------------------
// Win32 keyboard hooks

static BOOL WINAPI GetKeyboardStateHook(PBYTE lpKeyState)
{
    if (!lpKeyState) {
        return FALSE;
    }
    return read_unified_win32_state(lpKeyState) ? TRUE : FALSE;
}

static SHORT WINAPI GetAsyncKeyStateHook(int vKey)
{
    SHORT result = chain_GetAsyncKeyState(vKey);

    // Preserve the low-order "pressed since previous call" bit from Windows,
    // but make the high-order current-state bit a union with GetKeyboardState.
    if ((result & 0x8000) == 0 && vKey >= 0 && vKey < 256) {
        BYTE state[256];
        if (chain_GetKeyboardState(state) && (state[vKey] & 0x80)) {
            result = (SHORT)(result | 0x8000);
        }
    }

    return result;
}

static SHORT WINAPI GetKeyStateHook(int vKey)
{
    SHORT result = chain_GetKeyState(vKey);

    // Keep Windows' toggle bit exactly as it is; only widen the current-down
    // bit to the same union used elsewhere.
    if ((result & 0x8000) == 0 && vKey >= 0 && vKey < 256) {
        BYTE state[256];
        if (chain_GetKeyboardState(state) && (state[vKey] & 0x80)) {
            result = (SHORT)(result | 0x8000);
        }
    }

    return result;
}

// -----------------------------------------------------------------------------
// DirectInput keyboard hooks

static HRESULT WINAPI SetDataFormatHook(IDirectInputDevice8A* self, LPCDIDATAFORMAT lpdf)
{
    HRESULT result = chain_SetDataFormat(self, lpdf);
    if (result != DI_OK || !lpdf || lpdf->dwDataSize != 256) {
        return result;
    }

    for (size_t i = 0; i < sizeof(g_scan_to_di) / sizeof(g_scan_to_di[0]); ++i) {
        g_scan_to_di[i] = 0xFFFF;
    }

    DIPROPDWORD dip = {};
    dip.diph.dwSize = sizeof(dip);
    dip.diph.dwHeaderSize = sizeof(dip.diph);
    dip.diph.dwHow = DIPH_BYOFFSET;

    for (int i = 0; i < 256; ++i) {
        dip.diph.dwObj = (DWORD)i;
        if (self->GetProperty(DIPROP_SCANCODE, &dip.diph) != DI_OK) {
            continue;
        }

        const unsigned scan = normalize_dinput_scan_code(dip.dwData);
        if (scan < sizeof(g_scan_to_di) / sizeof(g_scan_to_di[0])) {
            g_scan_to_di[scan] = (uint16_t)i;
        }
    }

    g_di_map_ready = true;
    return result;
}

static void merge_vk_state_into_di(BYTE* diState)
{
    BYTE vkState[256];
    if (!read_unified_win32_state(vkState)) {
        return;
    }

    // Generic modifiers do not always map to one physical DirectInput key.
    // Treat the generic VK as an alias for both left/right variants.
    const bool shiftDown = (vkState[VK_SHIFT] & 0x80) ||
                           (vkState[VK_LSHIFT] & 0x80) ||
                           (vkState[VK_RSHIFT] & 0x80);
    const bool ctrlDown = (vkState[VK_CONTROL] & 0x80) ||
                          (vkState[VK_LCONTROL] & 0x80) ||
                          (vkState[VK_RCONTROL] & 0x80);
    const bool altDown = (vkState[VK_MENU] & 0x80) ||
                         (vkState[VK_LMENU] & 0x80) ||
                         (vkState[VK_RMENU] & 0x80);

    if (shiftDown) {
        const int l = di_index_for_vk(VK_LSHIFT);
        const int r = di_index_for_vk(VK_RSHIFT);
        if (l >= 0) diState[l] |= 0x80;
        if (r >= 0) diState[r] |= 0x80;
    }
    if (ctrlDown) {
        const int l = di_index_for_vk(VK_LCONTROL);
        const int r = di_index_for_vk(VK_RCONTROL);
        if (l >= 0) diState[l] |= 0x80;
        if (r >= 0) diState[r] |= 0x80;
    }
    if (altDown) {
        const int l = di_index_for_vk(VK_LMENU);
        const int r = di_index_for_vk(VK_RMENU);
        if (l >= 0) diState[l] |= 0x80;
        if (r >= 0) diState[r] |= 0x80;
    }

    for (int vk = 1; vk < 256; ++vk) {
        if (!(vkState[vk] & 0x80)) {
            continue;
        }

        // The generic modifier Vks were handled above.
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) {
            continue;
        }

        const int di = di_index_for_vk((UINT)vk);
        if (di >= 0 && di < 256) {
            diState[di] |= 0x80;
        }
    }
}

static HRESULT WINAPI GetDeviceStateHook(IDirectInputDevice8A* self, DWORD cbData, LPVOID lpvData)
{
    HRESULT result = chain_GetDeviceState(self, cbData, lpvData);

    if (result != DI_OK || cbData != 256 || !lpvData) {
        return result;
    }

    merge_vk_state_into_di((BYTE*)lpvData);
    return result;
}

static bool patch_keyboard_device_vtable(void*** iface)
{
    if (!iface || !*iface) {
        return false;
    }

    void** vtable = *iface;
    DWORD oldProtect = 0;
    if (!VirtualProtect(vtable + 9, 3 * sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        return false;
    }

    UpdateVTable(vtable[9], GetDeviceStateHook, chain_GetDeviceState);
    UpdateVTable(vtable[11], SetDataFormatHook, chain_SetDataFormat);

    DWORD ignored = 0;
    VirtualProtect(vtable + 9, 3 * sizeof(void*), oldProtect, &ignored);
    return true;
}

static HRESULT WINAPI CreateDeviceHook(IDirectInput8A* self,
                                       REFGUID rguid,
                                       LPDIRECTINPUTDEVICEA* lplpDirectInputDevice,
                                       LPUNKNOWN punkOuter)
{
    HRESULT result = chain_CreateDevice(self, rguid, lplpDirectInputDevice, punkOuter);
    if (result == DI_OK && lplpDirectInputDevice && *lplpDirectInputDevice && rguid == GUID_SysKeyboard) {
        auto iface = (void***)*lplpDirectInputDevice;
        patch_keyboard_device_vtable(iface);
    }
    return result;
}

static HRESULT WINAPI DirectInput8CreateHook(HINSTANCE hinst,
                                              DWORD dwVersion,
                                              REFIID riidltf,
                                              LPVOID* ppvOut,
                                              LPUNKNOWN punkOuter)
{
    HRESULT result = chain_DirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
    if (result == DI_OK && ppvOut && *ppvOut) {
        auto iface = (void***)*ppvOut;
        void** vtable = *iface;
        DWORD oldProtect = 0;
        if (VirtualProtect(vtable + 3, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            UpdateVTable(vtable[3], CreateDeviceHook, chain_CreateDevice);
            DWORD ignored = 0;
            VirtualProtect(vtable + 3, sizeof(void*), oldProtect, &ignored);
        }
    }
    return result;
}

static void Init()
{
    memset(g_scan_to_di, 0xFF, sizeof(g_scan_to_di));
    g_di_map_ready = false;

    detour_chain("user32.dll", 1,
                 "GetKeyboardState", GetKeyboardStateHook, &chain_GetKeyboardState,
                 nullptr);

    detour_chain("user32.dll", 1,
                 "GetAsyncKeyState", GetAsyncKeyStateHook, &chain_GetAsyncKeyState,
                 nullptr);

    detour_chain("user32.dll", 1,
                 "GetKeyState", GetKeyStateHook, &chain_GetKeyState,
                 nullptr);

    detour_chain("dinput8.dll", 1,
                 "DirectInput8Create", DirectInput8CreateHook, &chain_DirectInput8Create,
                 nullptr);
}

} // namespace UTI

extern "C" __declspec(dllexport) int __stdcall thcrap_plugin_init()
{
    HMODULE thcrap = GetModuleHandleA("thcrap.dll");
    if (!thcrap) {
        return 1;
    }

    *(FARPROC*)&UTI::detour_chain = GetProcAddress(thcrap, "detour_chain");
    *(FARPROC*)&UTI::runconfig_game_get = GetProcAddress(thcrap, "runconfig_game_get");

    if (!UTI::detour_chain || !UTI::runconfig_game_get) {
        return 1;
    }

    const char* game = UTI::runconfig_game_get();
    if (!UTI::is_supported_game(game)) {
        return 1;
    }

    return 0;
}

extern "C" __declspec(dllexport) void universal_touhou_input_mod_detour()
{
    UTI::Init();
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;
}
