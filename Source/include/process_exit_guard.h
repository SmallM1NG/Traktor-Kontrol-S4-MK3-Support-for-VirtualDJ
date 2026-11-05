// ZH: 独立进程的退出期限；只结束当前助手进程，不终止宿主或 NI 服务。
// EN: Process-local shutdown deadline. Never terminates the host or NI service.
#pragma once
#include <windows.h>

namespace shutdownlimit {
constexpr DWORD HelperMs = 2000, CleanupMs = 2000, PublisherMs = 2000;
class Guard {
    HANDLE done = nullptr, trigger = nullptr, parent = nullptr, thread = nullptr;
    DWORD budget = 0;
    static DWORD WINAPI run(void *raw) {
        auto &g = *static_cast<Guard *>(raw);
        HANDLE waits[] = {g.done, g.trigger, g.parent};
        DWORD result = WaitForMultipleObjects(g.parent ? 3 : 2, waits, FALSE, INFINITE);
        if (result == WAIT_OBJECT_0) return 0;
        if (result != WAIT_OBJECT_0+1 && result != WAIT_OBJECT_0+2) return 1;
        if (WaitForSingleObject(g.done, g.budget) == WAIT_OBJECT_0) return 0;
        // No logging/CRT cleanup here: a blocked writer or held lock must not block expiry.
        OutputDebugStringA("S4MK3 shutdown deadline exceeded; terminating current helper process.\n");
        TerminateProcess(GetCurrentProcess(), ERROR_TIMEOUT);
        return 1;
    }
  public:
    Guard() = default;
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;
    bool start(HANDLE stop, HANDLE owner, DWORD timeout) {
        if (thread || done) return false;
        budget = timeout;
        done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!done || !DuplicateHandle(GetCurrentProcess(), stop, GetCurrentProcess(), &trigger,
                                      SYNCHRONIZE, FALSE, 0)) return false;
        if (owner && !DuplicateHandle(GetCurrentProcess(), owner, GetCurrentProcess(), &parent,
                                      SYNCHRONIZE, FALSE, 0)) return false;
        thread = CreateThread(nullptr, 0, run, this, 0, nullptr);
        return thread != nullptr;
    }
    ~Guard() {
        if (done) SetEvent(done);
        if (thread) { WaitForSingleObject(thread, INFINITE); CloseHandle(thread); }
        if (parent) CloseHandle(parent);
        if (trigger) CloseHandle(trigger);
        if (done) CloseHandle(done);
    }
};
}
