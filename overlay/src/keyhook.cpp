#include "keyhook.h"

#include "animelogon/log.h"

namespace keyhook {
namespace {

Shared g_shared;
SRWLOCK g_latchLock = SRWLOCK_INIT;
wake::KeyLatch g_latch;
HANDLE g_thread = nullptr;
DWORD g_threadId = 0;
HANDLE g_ready = nullptr;
HHOOK g_hook = nullptr;
std::atomic<bool> g_installed{false};

bool ModifierDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// The hook callback. It decides one thing per key: pass it to Windows, or withhold it.
// It holds a short lock around the latch and never allocates, logs or blocks.
LRESULT CALLBACK Proc(int code, WPARAM wp, LPARAM lp) {
    if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wp, lp);
    const auto *k = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lp);
    const bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
    const bool up = wp == WM_KEYUP || wp == WM_SYSKEYUP;

    wake::KeyState s;
    s.armed = g_shared.armed.load();
    s.displayOff = g_shared.displayOff.load();

    const bool isWake = wake::IsWakeKey(k->vkCode, ModifierDown(VK_CONTROL),
                                        ModifierDown(VK_MENU),
                                        ModifierDown(VK_LWIN) || ModifierDown(VK_RWIN));

    AcquireSRWLockExclusive(&g_latchLock);
    const wake::KeyVerdict verdict = wake::DecideKey(g_latch, s, k->vkCode, down, up, isWake);
    ReleaseSRWLockExclusive(&g_latchLock);

    if (verdict == wake::KeyVerdict::Pass) return CallNextHookEx(nullptr, code, wp, lp);

    g_shared.withheld.fetch_add(1);
    if (verdict == wake::KeyVerdict::WithholdWake)
        if (HWND notify = g_shared.notify.load()) PostMessageW(notify, kWakeMessage, 0, 0);
    return 1;  // swallow it
}

DWORD WINAPI ThreadMain(LPVOID) {
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, Proc, GetModuleHandleW(nullptr), 0);
    if (!g_hook) ALOG(L"keyhook: SetWindowsHookEx failed (%lu) -- the wake key will reach the box", GetLastError());
    g_installed.store(g_hook != nullptr);
    SetEvent(g_ready);
    if (!g_hook) return 1;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // WM_QUIT ends the loop; nothing else is expected on this thread.
    }
    UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
    g_installed.store(false);
    return 0;
}

}  // namespace

Shared &State() { return g_shared; }

bool Install() {
    if (g_thread) return g_installed.load();
    g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, ThreadMain, nullptr, 0, &g_threadId);
    if (!g_thread) {
        ALOG(L"keyhook: could not start the hook thread (%lu)", GetLastError());
        if (g_ready) CloseHandle(g_ready);
        g_ready = nullptr;
        return false;
    }
    // The event stays open until Uninstall: a slow thread may still signal it.
    WaitForSingleObject(g_ready, 5000);
    return g_installed.load();
}

void Uninstall() {
    // Disarmed first, so a hook that outlives the wait below passes every key.
    g_shared.armed.store(false);
    if (!g_thread) return;
    // The thread may not have a message queue yet; keep asking until it ends.
    for (int i = 0; i < 50 && WaitForSingleObject(g_thread, 0) == WAIT_TIMEOUT; ++i) {
        PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
        WaitForSingleObject(g_thread, 100);
    }
    CloseHandle(g_thread);
    g_thread = nullptr;
    g_threadId = 0;
    if (g_ready) CloseHandle(g_ready);
    g_ready = nullptr;
}

bool Installed() { return g_installed.load(); }

void ResetLatch() {
    AcquireSRWLockExclusive(&g_latchLock);
    g_latch = wake::KeyLatch{};
    ReleaseSRWLockExclusive(&g_latchLock);
}

void DarkenLatch() {
    AcquireSRWLockExclusive(&g_latchLock);
    g_latch.litDisplay = false;
    ReleaseSRWLockExclusive(&g_latchLock);
}

}  // namespace keyhook
