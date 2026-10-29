// ZH: HID 输入适配：启动 Report 1/2 初值同步，以及旁路采样真实 Report 3 盘面角度。
// EN: HID input adapter: startup Report 1/2 synchronization plus passive physical-angle sampling from real Report 3.
#pragma once

// ZH: 冷启动输入同步：获取完整 HID 报告后交给原生输入入口；真实输入优先，每个报告只回灌一次。
// EN: Cold-start input synchronization: deliver complete HID reports to the native input entry, with real input taking precedence and each report injected once.

#include <windows.h>
#include <hidsdi.h>
#include <atomic>
#include <array>
#include <cstring>
#include <algorithm>
#include "bridge_ipc.h"
#include "jog_ring.h"
#include "runtime_utils.h"
#include "vdj_compatibility.h"
#include <MinHook.h>
#pragma comment(lib, "hid.lib")

namespace initial23
{
using Input = ULONG_PTR(__fastcall *)(void *, const BYTE *);

struct Report
{
    bool pending = false, ready = false, done = false;
    unsigned attempts = 0;
    ULONGLONG retry = 0, stamp = 0;
    std::array<BYTE, 79> packet{};
};

struct State
{
    SRWLOCK lock = SRWLOCK_INIT;
    HANDLE lease = nullptr;
    ULONG_PTR object = 0;
    ULONG_PTR raw = 0;
    unsigned long long generation = 0;
    std::array<Report, 2> reports{};
    WORD platter[2]{};
    bool platterValid = false;
};

inline State state;
inline Input original = nullptr;
inline void *target = nullptr;
inline std::atomic<bool> enabled{false};
inline std::array<std::atomic<unsigned>, 2> applied{}, natural{};
inline std::wstring logPath;
inline HANDLE workerStop = nullptr, workerWake = nullptr, workerThread = nullptr;

inline void log(const char *event, DWORD err = 0)
{
    if (FILE *f = openLiveLog(logPath.c_str()))
    {
        runtimediag::event(f, "DLL.input", err ? "WARN" : "INFO", event, err ? "error=%lu" : "", err);
        fclose(f);
    }
}

inline void logReport(const char *event, unsigned id, DWORD err = 0)
{
    if (FILE *f = openLiveLog(logPath.c_str()))
    {
        runtimediag::event(f, "DLL.input", err ? "WARN" : "INFO", event, err ? "report=%u error=%lu" : "report=%u", id,
                           err);
        fclose(f);
    }
}

template <class T> bool read(ULONG_PTR p, T &v)
{
    SIZE_T n = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(p), &v, sizeof(v), &n) && n == sizeof(v);
}

inline bool validPacket(const std::array<BYTE, 79> &p, BYTE requested)
{
    if (p[0] != requested)
        return false;
    if (requested == 1)
    {
        // ZH: 负载第 17 字节包含四个 2 位分配开关；第 18 字节第 6～7 位是曲线。
        // EN: Payload byte17: four 2-bit assignment switches; byte18 bits6..7: curve.
        // ZH: 保留其余所有位，它们来自真实硬件快照。
        // EN: Preserve every other bit: it belongs to the actual hardware snapshot.
        for (unsigned shift = 0; shift < 8; shift += 2)
            if (((p[18] >> shift) & 3) > 2)
                return false;
        return (p[19] >> 6) <= 2;
    }
    if (requested != 2)
        return false;
    for (unsigned i = 0; i < 39; ++i)
    {
        unsigned v = p[1 + 2 * i] | (unsigned(p[2 + 2 * i]) << 8);
        if (v > 4095)
            return false;
    }
    return true;
}

// ZH: HID 能力值表示最大缓冲区大小，不是各报告实际返回的长度。
// EN: HID caps specify maximum buffer size, not each report's returned length.
inline bool validTransfer(const std::array<BYTE, 79> &p, BYTE requested, DWORD got)
{
    const DWORD minimum = requested == 1 ? 23 : 79;
    return got >= minimum && got <= p.size() && validPacket(p, requested);
}

inline void resetReports(ULONGLONG now)
{
    state.platterValid = false;
    for (auto &r : state.reports)
    {
        r = Report{};
        r.pending = true;
        r.retry = now;
    }
}

inline ULONG_PTR __fastcall input(void *obj, const BYTE *data)
{
    std::array<BYTE, 79> local{};
    bool replace = false, notify = false;
    if (enabled.load(std::memory_order_acquire) && TryAcquireSRWLockExclusive(&state.lock))
    {
        const ULONG_PTR p = reinterpret_cast<ULONG_PTR>(obj);
        ULONG_PTR vt = 0, raw = 0;
        DWORD size = 0;
        BYTE id = 0;
        bool match = read(p, vt) && vt == vdjcompat::addresses.inputVtable &&
                     read(p + vdjcompat::addresses.inputReportSize, size) && size == 78 &&
                     read(p + vdjcompat::addresses.inputHandle, raw) && raw &&
                     read(reinterpret_cast<ULONG_PTR>(data) - 1, id) && (id >= 1 && id <= 3);
        if (match)
        {
            HANDLE h = reinterpret_cast<HANDLE>(raw);
            if (state.object != p || state.raw != raw || !state.lease || !samePipeObject(state.lease, h))
            {
                HIDD_ATTRIBUTES a{};
                a.Size = sizeof(a);
                HANDLE dup = nullptr;
                if (HidD_GetAttributes(h, &a) && a.VendorID == 0x17cc && a.ProductID == 0x1720 &&
                    DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
                {
                    if (state.lease)
                        CloseHandle(state.lease);
                    state.lease = dup;
                    state.object = p;
                    state.raw = raw;
                    ++state.generation;
                    resetReports(GetTickCount64());
                    notify = true;
                }
                else
                    match = false;
            }
            if (match && state.object == p)
            {
                // ZH: 在启动报告替换之前采样真实负载；不增加 USB 读取，不在输入回调打印日志。
                // EN: Sample the real payload before startup substitution; no extra USB reads or callback logging.
                if (id == 3)
                {
                    BYTE payload[48]{};
                    SIZE_T got = 0;
                    WORD angles[2]{};
                    if (ReadProcessMemory(GetCurrentProcess(), data, payload, sizeof(payload), &got) &&
                        got == sizeof(payload) && physicalJog::decode(payload, sizeof(payload), angles))
                    {
                        state.platter[0] = angles[0];
                        state.platter[1] = angles[1];
                        state.platterValid = true;
                    }
                }
                if (id == 1 || id == 2)
                {
                    auto &r = state.reports[id - 1];
                    if (!r.done)
                    {
                        ++natural[id - 1];
                        notify = true;
                    }
                    r.done = true;
                    r.ready = false;
                    r.pending = false;
                }
                else
                    for (unsigned i = 0; i < state.reports.size(); ++i)
                    {
                        auto &r = state.reports[i];
                        if (r.done || !r.ready)
                            continue;
                        if (GetTickCount64() - r.stamp <= 250)
                        {
                            local = r.packet;
                            replace = true;
                            r.done = true;
                            r.ready = false;
                            r.pending = false;
                            ++applied[i];
                            notify = true;
                            break;
                        }
                        r.ready = false;
                        r.pending = r.attempts < 3;
                        r.retry = GetTickCount64() + 100;
                        notify = true;
                    }
            }
        }
        ReleaseSRWLockExclusive(&state.lock);
    }
    if (notify && workerWake)
        SetEvent(workerWake);
    return original(obj, replace ? local.data() + 1 : data);
}

// ZH: 专用输入线程读取报告；状态发布和 HID 回调都不等待 USB 完成。
// EN: A dedicated input worker reads reports; neither state publishing nor HID callbacks wait for USB completion.
inline void readReport(unsigned index)
{
    if (!enabled.load() || index >= 2)
        return;
    HANDLE h = nullptr;
    unsigned long long generation = 0;
    AcquireSRWLockExclusive(&state.lock);
    auto &selected = state.reports[index];
    if (state.lease && selected.pending && !selected.done && GetTickCount64() >= selected.retry)
    {
        ++selected.attempts;
        if (DuplicateHandle(GetCurrentProcess(), state.lease, GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS))
        {
            selected.pending = false;
            generation = state.generation;
        }
        else
        {
            selected.pending = selected.attempts < 3;
            selected.retry = GetTickCount64() + 1000;
        }
    }
    ReleaseSRWLockExclusive(&state.lock);
    if (!h)
        return;
    const BYTE requested = static_cast<BYTE>(index + 1);
    std::array<BYTE, 79> packet{};
    packet[0] = requested;
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD got = 0, error = 0;
    bool ok = false;
    if (ov.hEvent)
    {
        // ZH: 该输入报告 IOCTL 定义为 HID_OUT_CTL_CODE(104)，来源是 Windows SDK 的 hidclass.h。
        // EN: IOCTL_HID_GET_INPUT_REPORT = HID_OUT_CTL_CODE(104), from Windows SDK hidclass.h.
        BOOL started =
            DeviceIoControl(h, 0x000b01a2, nullptr, 0, packet.data(), static_cast<DWORD>(packet.size()), &got, &ov);
        error = started ? 0 : GetLastError();
        if (started)
            ok = true;
        else if (error == ERROR_IO_PENDING)
        {
            HANDLE waits[] = {ov.hEvent, workerStop};
            const DWORD waited =
                workerStop ? WaitForMultipleObjects(2, waits, FALSE, 250) : WaitForSingleObject(ov.hEvent, 250);
            if (waited == WAIT_OBJECT_0)
            {
                ok = GetOverlappedResult(h, &ov, &got, FALSE) != FALSE;
                error = ok ? 0 : GetLastError();
            }
            else
            {
                CancelIoEx(h, &ov);
                GetOverlappedResult(h, &ov, &got, TRUE);
                error = waited == WAIT_OBJECT_0 + 1 ? ERROR_OPERATION_ABORTED : WAIT_TIMEOUT;
            }
        }
        CloseHandle(ov.hEvent);
    }
    else
        error = GetLastError();
    CloseHandle(h);
    if (ok && (!validTransfer(packet, requested, got)))
    {
        ok = false;
        error = ERROR_INVALID_DATA;
    }
    AcquireSRWLockExclusive(&state.lock);
    auto &r = state.reports[index];
    if (enabled.load() && state.generation == generation && !r.done)
    {
        if (ok)
        {
            r.packet = packet;
            r.stamp = GetTickCount64();
            r.ready = true;
        }
        else
        {
            r.pending = r.attempts < 3;
            r.retry = GetTickCount64() + 1000;
        }
    }
    ReleaseSRWLockExclusive(&state.lock);
    logReport(ok ? "snapshot_ready" : "snapshot_read_failed", index + 1, error);
}

// ZH: 专用线程按报告到期时间休眠；无任务时完全等待通知，不随发布周期轮询 USB。
// EN: The dedicated worker sleeps until the next report deadline; when idle it waits for notification instead of polling USB.
inline void reportCounters()
{
    static std::array<unsigned, 2> lastApplied{}, lastNatural{};
    for (unsigned i = 0; i < 2; ++i)
    {
        unsigned a = applied[i].load(), n = natural[i].load();
        if (a != lastApplied[i])
        {
            logReport("snapshot_delivered_to_native_mapping", i + 1);
            lastApplied[i] = a;
        }
        if (n != lastNatural[i])
        {
            logReport("real_report_received_snapshot_not_needed", i + 1);
            lastNatural[i] = n;
        }
    }
}

inline DWORD WINAPI reader(void *)
{
    runtimediag::pathEvent(logPath, "DLL.input", "INFO", "reader_worker_started");
    try
    {
        for (;;)
        {
            if (WaitForSingleObject(workerStop, 0) != WAIT_TIMEOUT)
                break;
            readReport(0);
            readReport(1);
            reportCounters();
            DWORD wait = INFINITE;
            const auto now = GetTickCount64();
            AcquireSRWLockShared(&state.lock);
            if (enabled.load() && state.lease)
                for (const auto &r : state.reports)
                    if (r.pending && !r.done)
                        wait = (std::min)(wait, r.retry <= now ? DWORD(0)
                                                               : static_cast<DWORD>((std::min)(ULONGLONG(MAXDWORD - 1),
                                                                                               r.retry - now)));
            ReleaseSRWLockShared(&state.lock);
            HANDLE waits[] = {workerStop, workerWake};
            const auto result = WaitForMultipleObjects(2, waits, FALSE, wait);
            if (result == WAIT_OBJECT_0 || result == WAIT_FAILED)
                break;
        }
    }
    catch (...)
    {
        log("reader_worker_exception", ERROR_UNHANDLED_EXCEPTION);
    }
    runtimediag::pathEvent(logPath, "DLL.input", "INFO", "reader_worker_stopped");
    return 0;
}

inline bool startWorker()
{
    if (workerThread)
        return false;
    if (!workerStop)
        workerStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!workerWake)
        workerWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!workerStop || !workerWake)
    {
        log("reader_events_create_failed", GetLastError());
        return false;
    }
    ResetEvent(workerStop);
    ResetEvent(workerWake);
    workerThread = CreateThread(nullptr, 0, reader, nullptr, 0, nullptr);
    if (!workerThread)
    {
        log("reader_thread_create_failed", GetLastError());
        return false;
    }
    return true;
}

inline void stopWorker()
{
    if (!workerThread)
        return;
    SetEvent(workerStop);
    SetEvent(workerWake);
    if (WaitForSingleObject(workerThread, 3000) == WAIT_OBJECT_0)
    {
        CloseHandle(workerThread);
        workerThread = nullptr;
    }
    else
        log("reader_stop_timeout_storage_retained", ERROR_TIMEOUT);
    // ZH: 事件保留至进程退出，避免迟到的原生回调使用已关闭的事件句柄。
    // EN: Retain events until process exit so late native callbacks never use closed event handles.
}

// ZH: 保留发布者接口，但不在发布线程执行任何 HID I/O 或日志写入。
// EN: Preserve the publisher interface without HID I/O or log writes on the publisher thread.
inline void tick()
{
}

inline bool start(const std::wstring &path)
{
    logPath = path;
    HMODULE pinned = nullptr;
    // ZH: 延迟回调及插件卸载期间必须保持跳板与回调存储有效。
    // EN: Keep trampoline and callback storage alive through late callbacks and plugin unload.
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&input), &pinned))
    {
        log("module_pin_failed", GetLastError());
        return false;
    }
    if (target)
    {
        log("reload_requires_vdj_restart");
        return false;
    }
    target = reinterpret_cast<void *>(vdjcompat::addresses.input);
    if (!target || !vdjcompat::addresses.inputVtable)
    {
        log("input_feature_location_rejected");
        return false;
    }
    MH_STATUS r = MH_Initialize();
    if (r != MH_OK && r != MH_ERROR_ALREADY_INITIALIZED)
    {
        log("hook_initialize_failed", static_cast<DWORD>(r));
        return false;
    }
    r = MH_CreateHook(target, reinterpret_cast<void *>(&input), reinterpret_cast<void **>(&original));
    if (r != MH_OK)
    {
        log("hook_create_failed", static_cast<DWORD>(r));
        return false;
    }
    // ZH: 先创建通知事件再开放回调，避免回调与事件句柄初始化并发。
    // EN: Create notification events before enabling callbacks so handle initialization cannot race with input.
    if (!startWorker())
        return false;
    enabled.store(true);
    r = MH_EnableHook(target);
    if (r != MH_OK)
    {
        enabled.store(false);
        stopWorker();
        log("hook_enable_failed", static_cast<DWORD>(r));
        return false;
    }
    log("waiting_for_s4_input");
    return true;
}

// ZH: 短共享锁复制一对角度；后续相位计算在锁外、发布线程内完成。
// EN: Copy the angle pair under a short shared lock; phase computation runs unlocked in the publisher.
inline void physicalAngles(phase18::Frame &frame)
{
    WORD angles[2]{};
    AcquireSRWLockShared(&state.lock);
    bool valid = enabled.load() && state.lease && state.platterValid;
    angles[0] = state.platter[0];
    angles[1] = state.platter[1];
    ReleaseSRWLockShared(&state.lock);
    physicalJog::apply(frame, angles, valid);
}

inline void stop()
{
    enabled.store(false);
    stopWorker();
    if (original)
    {
        auto r = MH_DisableHook(target);
        log("hook_disabled", static_cast<DWORD>(r));
    }
    AcquireSRWLockExclusive(&state.lock);
    if (state.lease)
        CloseHandle(state.lease);
    state.lease = nullptr;
    for (auto &report : state.reports)
    {
        report.pending = false;
        report.ready = false;
    }
    ReleaseSRWLockExclusive(&state.lock);
    // ZH: 回调可能尚未返回时不得释放跳板；固定存储在进程退出时回收。
    // EN: Do not free the trampoline while a callback may still be returning. Pinned storage is reclaimed at process exit.
}
}
