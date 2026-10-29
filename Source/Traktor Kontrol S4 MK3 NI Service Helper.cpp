// ZH: NI 助手进程入口：独立日志、严格参数解析、父进程/会话校验、单实例租约及连接重建。
// EN: NI helper entry: independent logging, strict arguments, parent/session validation, single-instance lease and reconnection.
#include "release_identity.h"
#include "runtime_utils.h"
#include <windows.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <new>
#include <cmath>
#include <cerrno>
#include "ni_service_session.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

struct View
{
    const phase18::Frame *p = nullptr;

    ~View()
    {
        if (p)
            UnmapViewOfFile(p);
    }
};

struct Lease
{
    HANDLE h = nullptr;
    bool owned = false;

    ~Lease()
    {
        if (owned)
            ReleaseMutex(h);
        if (h)
            CloseHandle(h);
    }
};

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return 2;
    wchar_t path[32768];
    DWORD n = GetModuleFileNameW(nullptr, path, 32768);
    if (!n || n >= 32768)
        return 2;
    std::wstring log(path, n);
    auto slash = log.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return 2;
    log.resize(slash + 1);
    log += S4_PHASE_LOG;
    // ZH: 日志先于参数与权限验证打开，启动失败也能留下原因。看门狗子进程不轮换主助手的日志。
    // EN: Open logging before argument and privilege checks so startup failures are recorded. The watchdog does not rotate the main helper log.
    const bool watchdog = argc == 5 && !wcscmp(argv[1], L"--ni-watchdog");
    FILE *file = openLiveLog(log.c_str());
    if (!file)
    {
        OutputDebugStringA("S4MK3 EXE log_open_failed\n");
        LocalFree(argv);
        return 3;
    }

    struct CloseLog
    {
        FILE *file;

        ~CloseLog()
        {
            fclose(file);
        }
    } closeLog{file};

    runtimediag::section(file, watchdog ? "WATCHDOG START" : "HELPER START");
    runtimediag::Exit exit{file, "EXE"};
    auto note = [&](const char *text, LONG code = 0) {
        runtimediag::event(file, "EXE", code < 0 ? "ERROR" : "INFO", text, code ? "value=%ld" : "", code);
    };
    note(watchdog ? "watchdog_process_started" : "helper_process_started");
    runtimediag::event(file, "EXE", "INFO", "runtime_identity", "version=%s pointer_bits=%u", S4_RELEASE_VERSION,
                       unsigned(sizeof(void *) * 8));
    if (watchdog)
    {
        int result = ni24::watchdogArgs(argv + 2, file);
        LocalFree(argv);
        exit.finish(result, "watchdog_complete");
        return result;
    }
    DWORD pid = 0, duration = 0;
    bool apply = false, haveDuration = false, valid = true;
    std::wstring token;
    for (int i = 1; i < argc && valid; ++i)
    {
        if (!wcscmp(argv[i], L"--apply"))
        {
            if (apply)
            {
                valid = false;
                break;
            }
            apply = true;
            continue;
        }
        if (!wcscmp(argv[i], L"--session"))
        {
            if (!token.empty() || i + 1 >= argc)
            {
                valid = false;
                break;
            }
            token = argv[++i];
            valid = phase18::validToken(token.c_str());
            continue;
        }
        bool isPid = !wcscmp(argv[i], L"--pid"), isDuration = !wcscmp(argv[i], L"--duration");
        if ((!isPid && !isDuration) || i + 1 >= argc || (isPid && pid) || (isDuration && haveDuration))
        {
            valid = false;
            break;
        }
        const wchar_t *text = argv[++i];
        if (!*text)
        {
            valid = false;
            break;
        }
        for (const wchar_t *p = text; *p; ++p)
            if (*p < L'0' || *p > L'9')
                valid = false;
        errno = 0;
        wchar_t *end = nullptr;
        unsigned long value = wcstoul(text, &end, 10);
        if (errno || *end || !value)
        {
            valid = false;
            break;
        }
        if (isPid)
            pid = value;
        else
        {
            if (value > 60)
            {
                valid = false;
                break;
            }
            duration = value * 1000;
            haveDuration = true;
        }
    }
    LocalFree(argv);
    if (!valid || !pid || token.empty())
    {
        exit.finish(2, "invalid_arguments");
        return 2;
    }
    runtimediag::event(file, "EXE", "INFO", "publisher_identity", "vdj_pid=%lu apply=%u bounded_ms=%lu", pid,
                       apply ? 1u : 0u, duration);
    HRESULT result = E_UNEXPECTED;
    try
    {
        // ZH: 只接受持有有效发布会话的 VDJ；命令行不能直接获得硬件写权限。
        // EN: Accept only VDJ with a valid publishing session; command-line arguments alone cannot authorize hardware writes.
        result = [&]() -> HRESULT {
            auto failure = [&](const char *reason, HRESULT hr) {
                runtimediag::event(file, "EXE", "ERROR", reason, "hresult=0x%08lX win_error=%lu",
                                   static_cast<unsigned long>(hr), GetLastError());
                return hr;
            };
            BH parent(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
            if (!parent.h)
                return failure("parent_open_failed", E_ACCESSDENIED);
            if (!bridgeVdj(parent.h))
                return failure("parent_identity_rejected", E_ACCESSDENIED);
            ULONGLONG created = phase18::creation(parent.h);
            if (!created)
                return failure("parent_creation_time_failed", E_FAIL);
            wchar_t mappingName[128], stopName[128];
            phase18::name(mappingName, L"Phase", pid, token.c_str());
            phase18::name(stopName, L"Stop", pid, token.c_str());
            BH stop(OpenEventW(SYNCHRONIZE, FALSE, stopName));
            if (!stop.h)
                return failure("stop_event_open_failed", HRESULT_FROM_WIN32(GetLastError()));
            wchar_t changedName[128];
            phase18::name(changedName, L"Changed", pid, token.c_str());
            BH changed(OpenEventW(SYNCHRONIZE, FALSE, changedName));
            note(changed.h ? "publisher_notification_enabled" : "publisher_notification_poll_fallback");
            wchar_t dataName[128];
            phase18::name(dataName, L"MotorData", pid, token.c_str());
            BH dataReady(OpenEventW(SYNCHRONIZE, FALSE, dataName));
            wchar_t refreshName[128];
            phase18::name(refreshName, L"RefreshData", pid, token.c_str());
            BH refresh(OpenEventW(EVENT_MODIFY_STATE, FALSE, refreshName));
            BH mapping(OpenFileMappingW(FILE_MAP_READ, FALSE, mappingName));
            if (!mapping.h)
                return failure("publisher_mapping_open_failed", HRESULT_FROM_WIN32(GetLastError()));
            View view;
            view.p = static_cast<const phase18::Frame *>(
                MapViewOfFile(mapping.h, FILE_MAP_READ, 0, 0, sizeof(phase18::Frame)));
            if (!view.p)
                return failure("publisher_mapping_view_failed", HRESULT_FROM_WIN32(GetLastError()));
            wchar_t parentPath[32768]{};
            DWORD count = 32768;
            if (!QueryFullProcessImageNameW(parent.h, 0, parentPath, &count))
                return failure("parent_path_query_failed", E_ACCESSDENIED);
            const wchar_t *leaf = wcsrchr(parentPath, L'\\');
            if (!leaf || _wcsicmp(leaf + 1, L"virtualdj.exe"))
                return failure("parent_filename_rejected", E_ACCESSDENIED);
            Lease lease;
            lease.h = CreateMutexW(nullptr, FALSE, L"Global\\S4NIAbsoluteBridgeV1");
            if (!lease.h)
                return failure("hardware_mutex_create_failed", HRESULT_FROM_WIN32(GetLastError()));
            HANDLE acquire[] = {stop.h, parent.h, lease.h};
            DWORD acquired = WaitForMultipleObjects(3, acquire, FALSE, 3000);
            if (acquired == WAIT_OBJECT_0 || acquired == WAIT_OBJECT_0 + 1)
            {
                note(acquired == WAIT_OBJECT_0 ? "plugin_stop_before_attach" : "parent_ended_before_attach");
                return S_OK;
            }
            if (acquired != WAIT_OBJECT_0 + 2 && acquired != WAIT_ABANDONED_0 + 2)
                return failure("hardware_mutex_busy", HRESULT_FROM_WIN32(ERROR_BUSY));
            lease.owned = true;

            note(apply ? "continuous_helper_ready" : "readonly_helper_ready");
            ULONGLONG deadline = duration ? GetTickCount64() + duration : 0, lastCoherent = GetTickCount64();
            runtimetime::Retry retry;
            int previous = -1;
            LONG sessions = 0;
            ULONGLONG lastStatus = GetTickCount64();
            HANDLE waits[] = {stop.h, parent.h, changed.h};
            while (!deadline || GetTickCount64() < deadline)
            {
                DWORD parentState = WaitForMultipleObjects(2, waits, FALSE, 0);
                if (parentState == WAIT_FAILED)
                    return failure("parent_wait_failed", HRESULT_FROM_WIN32(GetLastError()));
                bool ended = parentState != WAIT_TIMEOUT;
                phase18::Frame a{}, b{};
                memcpy(&a, view.p, sizeof(a));
                MemoryBarrier();
                memcpy(&b, view.p, sizeof(b));
                ULONGLONG now = GetTickCount64();
                bool coherent = false;
                auto action = phase18::decide(a, b, pid, created, now, 0, ended, coherent);
                if (coherent)
                    lastCoherent = now;
                if (now - lastStatus >= 30000)
                {
                    runtimediag::event(file, "EXE", "INFO", "publisher_wait_heartbeat",
                                       "vdj_pid=%lu generation=%llu state=%lu valid=%lu coherent=%u frame_age_ms=%llu",
                                       pid, a.generation, a.state, a.valid, coherent ? 1u : 0u,
                                       now >= a.tick ? now - a.tick : 0);
                    lastStatus = now;
                }
                if (action == phase18::Action::Stop || now - lastCoherent > 3000)
                {
                    note(ended ? (parentState == WAIT_OBJECT_0 ? "plugin_stop_event" : "vdj_process_ended")
                               : "publisher_stopped_or_stale");
                    return S_OK;
                }
                if (action == phase18::Action::Attach && retry.ready(a.generation, now))
                {
                    BridgeSession session;
                    session.pid = pid;
                    session.created = created;
                    session.generation = a.generation;
                    session.sessionToken = token;
                    session.source = view.p;
                    session.apply = apply;
                    session.telemetry = file;
                    CloseHandle(session.stop.h);
                    session.stop.h = nullptr;
                    if (!DuplicateHandle(GetCurrentProcess(), stop.h, GetCurrentProcess(), &session.stop.h, SYNCHRONIZE,
                                         FALSE, 0))
                        return failure("session_stop_duplicate_failed", E_FAIL);
                    session.durationMs =
                        deadline ? static_cast<DWORD>((std::min)(ULONGLONG(60000), deadline - now)) : 0;
                    note("attach_generation", static_cast<LONG>(a.generation));
                    const auto attached = GetTickCount64();
                    HRESULT hr = session.execute();
                    ++sessions;
                    note("session_end", hr);
                    if (FAILED(hr))
                        return hr;
                    if (hr == 2)
                    {
                        note("lifecycle_complete", sessions);
                        return S_OK;
                    }
                    note("waiting_for_revalidated_connection");
                    retry.failed(a.generation, GetTickCount64(), GetTickCount64() - attached >= 5000);
                    note("same_generation_retry_delay_ms", static_cast<LONG>(retry.delay));
                    previous = -1;
                }
                else if (previous != static_cast<int>(action))
                {
                    note("waiting_for_active_publisher");
                    previous = static_cast<int>(action);
                }
                // ZH: 新连接通知立即唤醒；仅同代次失败采用退避，不以固定 100ms 轮询阻挡启动。
                // EN: Wake immediately on new-connection notifications; back off only same-generation failures instead of fixed 100ms startup polling.
                DWORD waitMs = changed.h ? 1000u : 100u;
                if (!coherent)
                    waitMs = framewake::GuardMs;
                const auto waitNow = GetTickCount64();
                if (action == phase18::Action::Attach && a.generation == retry.generation)
                    waitMs =
                        (std::min)(waitMs, retry.next > waitNow ? static_cast<DWORD>(retry.next - waitNow) : DWORD(0));
                HANDLE pending[4]{stop.h, parent.h, nullptr, nullptr};
                DWORD pendingCount = 2;
                if (changed.h)
                    pending[pendingCount++] = changed.h;
                // ZH: 只在等完整帧时消费电机通知；同代次退避期间只等生命周期变化，避免逐帧空唤醒。
                // EN: Consume motor notifications only while awaiting a coherent frame; retry backoff waits on lifecycle events to avoid per-frame idle wakes.
                if (dataReady.h && (!coherent || action == phase18::Action::Idle))
                    pending[pendingCount++] = dataReady.h;
                if (!coherent && refresh.h)
                    SetEvent(refresh.h);
                DWORD waited = WaitForMultipleObjects(pendingCount, pending, FALSE, waitMs);
                if (waited == WAIT_OBJECT_0 || waited == WAIT_OBJECT_0 + 1)
                {
                    note(waited == WAIT_OBJECT_0 ? "plugin_stop_event" : "vdj_process_ended");
                    return S_OK;
                }
                if (waited == WAIT_FAILED)
                    return failure("publisher_wait_failed", HRESULT_FROM_WIN32(GetLastError()));
            }
            note("bounded_readonly_or_test_complete");
            return S_OK;
        }();
    }
    catch (...)
    {
        note("helper_exception", E_UNEXPECTED);
        result = E_UNEXPECTED;
    }
    note("helper_exit", result);
    exit.finish(FAILED(result) ? 1 : 0, "helper_complete");
    return FAILED(result) ? 1 : 0;
}
