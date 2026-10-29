// ZH: NI服务会话协调：设备对象、命令工作线程、电机策略、触觉与灯环通道、独立退出看门狗。
// EN: NI service session coordination: device objects, command workers, motor policy, haptic/ring lanes and independent shutdown watchdog.
#pragma once
#include <windows.h>

struct BH
{
    HANDLE h = nullptr;
    BH() = default;

    explicit BH(HANDLE v) : h(v)
    {
    }

    ~BH()
    {
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }

    BH(const BH &) = delete;
    BH &operator=(const BH &) = delete;
};

// ZH: 命令串行写入器和独立周期通道。电机请求优先；超时只取消当前 I/O，退出先停止生产者再回收管道。
// EN: Serialized command writer and independent periodic lanes. Prioritize motor requests, cancel only current I/O, and stop producers before releasing the pipe.

#include <windows.h>
#include "runtime_utils.h"
#include <cstdio>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <algorithm>

namespace controlwork
{
// ZH: 普通命令由一个常驻线程写入；每个调用者最多保留一个
// EN: One long-lived writer owns normal command I/O. There is at most one
// ZH: 未完成请求，电机请求优先于排队的触觉请求。
// EN: outstanding request per caller; motor requests precede queued haptic work.
class Pipe
{
    struct Request
    {
        std::vector<BYTE> bytes;
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD error = 0;
        ULONGLONG started = 0, queued = GetTickCount64();
        bool cancelled = false;
        std::function<bool()> current;

        ~Request()
        {
            if (done)
                CloseHandle(done);
        }
    };

    HANDLE pipe;
    std::function<bool()> allowed;
    std::mutex mutex;
    std::atomic<ULONGLONG> completed{0}, timeouts{0}, maxQueue{0}, maxWrite{0};
    std::atomic<DWORD> tid{0};

    static void maximum(std::atomic<ULONGLONG> &into, ULONGLONG value)
    {
        auto old = into.load();
        while (value > old && !into.compare_exchange_weak(old, value))
        {
        }
    }

    std::condition_variable wake;
    std::deque<std::shared_ptr<Request>> motor, haptic;
    std::shared_ptr<Request> active;
    bool stopping = false;
    std::thread worker;

    void run()
    {
        tid = GetCurrentThreadId();
        for (;;)
        {
            std::shared_ptr<Request> r;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [&] {
                    return stopping || !motor.empty() || !haptic.empty();
                });
                if (stopping)
                    break;
                auto &queue = motor.empty() ? haptic : motor;
                r = queue.front();
                queue.pop_front();
                active = r;
                r->started = GetTickCount64();
            }
            maximum(maxQueue, r->started - r->queued);
            DWORD count = 0, error = 0;
            if (!allowed())
                error = ERROR_OPERATION_ABORTED;
            else if (r->current && !r->current())
                error = ERROR_RETRY;
            else if ([&] {
                         std::lock_guard<std::mutex> lock(mutex);
                         return r->cancelled || stopping;
                     }())
                error = ERROR_OPERATION_ABORTED;
            else if (!WriteFile(pipe, r->bytes.data(), static_cast<DWORD>(r->bytes.size()), &count, nullptr))
                error = GetLastError();
            else if (count != r->bytes.size())
                error = ERROR_WRITE_FAULT;
            maximum(maxWrite, GetTickCount64() - r->started);
            ++completed;
            {
                std::lock_guard<std::mutex> lock(mutex);
                r->error = r->cancelled ? ERROR_TIMEOUT : error;
                active.reset();
                SetEvent(r->done);
            }
        }
    }

  public:
    Pipe(HANDLE handle, std::function<bool()> validate)
        : pipe(handle), allowed(std::move(validate)), worker([this] {
              run();
          })
    {
    }

    Pipe(const Pipe &) = delete;
    Pipe &operator=(const Pipe &) = delete;

    // ZH: 调用者总等待上限 200ms；超时后由写线程继续持有请求，取消完成前不得释放数据。
    // EN: Limit caller waiting to 200ms total; the writer retains timed-out requests until cancellation finishes.
    bool send(const void *bytes, size_t size, bool critical, std::function<bool()> current = {})
    {
        if (!size || size > MAXDWORD)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return false;
        }
        auto r = std::make_shared<Request>();
        if (!r->done)
            return false;
        r->current = std::move(current);
        const BYTE *begin = static_cast<const BYTE *>(bytes);
        r->bytes.assign(begin, begin + size);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping)
            {
                SetLastError(ERROR_OPERATION_ABORTED);
                return false;
            }
            // ZH: 已取消的在途写入尚未返回时，不继续堆积或重放新命令。
            // EN: Do not queue or replay new commands while a cancelled write is still outstanding.
            if (active && active->cancelled)
            {
                SetLastError(ERROR_TIMEOUT);
                return false;
            }
            (critical ? motor : haptic).push_back(r);
        }
        wake.notify_one();
        const DWORD waited = WaitForSingleObject(r->done, 200);
        if (waited != WAIT_OBJECT_0)
        {
            const DWORD error = waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
            std::lock_guard<std::mutex> lock(mutex);
            if (WaitForSingleObject(r->done, 0) != WAIT_OBJECT_0)
            {
                r->cancelled = true;
                r->error = error;
                ++timeouts;
                if (active == r)
                    CancelSynchronousIo(worker.native_handle());
                else
                    for (auto *queue : {&motor, &haptic})
                    {
                        auto at = std::find(queue->begin(), queue->end(), r);
                        if (at != queue->end())
                            queue->erase(at);
                    }
                SetLastError(error);
                return false;
            }
        }
        SetLastError(r->error);
        return !r->error;
    }

    void log(FILE *file)
    {
        if (file)
        {
            liveLogPrintf(
                file,
                "tick=%llu COMMAND_WORKER tid=%lu completed=%llu timeouts=%llu max_queue_ms=%llu max_write_ms=%llu\n",
                GetTickCount64(), tid.load(), completed.load(), timeouts.load(), maxQueue.load(), maxWrite.load());
            fflush(file);
        }
    }

    size_t pending()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return motor.size() + haptic.size();
    }

    // ZH: 唤醒所有排队调用者并合并写线程；销毁句柄前必须完成 join。
    // EN: Wake queued callers and join the writer before destroying handles.
    void close()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            for (auto *queue : {&motor, &haptic})
            {
                for (auto &r : *queue)
                {
                    r->error = ERROR_OPERATION_ABORTED;
                    SetEvent(r->done);
                }
                queue->clear();
            }
            if (active)
                CancelSynchronousIo(worker.native_handle());
        }
        wake.notify_all();
        // ZH: 返回超时不等于驱动已取消；回收阶段重复取消并等待，保证会话引用和数据不悬空。
        // EN: A caller timeout does not imply driver cancellation; retry cancellation while joining to retain session references safely.
        if (worker.joinable())
        {
            while (WaitForSingleObject(worker.native_handle(), 100) == WAIT_TIMEOUT)
                CancelSynchronousIo(worker.native_handle());
            worker.join();
        }
    }

    ~Pipe()
    {
        close();
    }
};

template <class Fresh, class Beat>
bool sendMotor(Pipe &pipe, const DWORD (&words)[3], const std::function<bool()> &current, Fresh fresh, Beat beat)
{
    if (!pipe.send(words, sizeof(words), true, current))
        return false;
    if (!fresh())
    {
        SetLastError(ERROR_OPERATION_ABORTED);
        return false;
    }
    beat();
    return true;
}

// ZH: 独立周期通道持有各自状态，退出时先合并这些线程，
// EN: Independent periodic lanes own their state; shutdown joins them before
// ZH: 再销毁共享管道、NI 句柄、源映射或看门狗。
// EN: the shared pipe, NI handles, source mapping or watchdog can be destroyed.
class Lane
{
    HANDLE halt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread worker;
    FILE *telemetry = nullptr;
    const char *name = "control_lane";

  public:
    explicit Lane(FILE *file = nullptr, const char *lane = "control_lane") : telemetry(file), name(lane)
    {
    }

    std::atomic<bool> failed{false};
    std::atomic<ULONGLONG> dataWakes{0}, timeoutChecks{0};

    // ZH: 工作结束即等新数据；仅超时执行安全检查。独立停止事件先于数据，不让失效线程静默退出。
    // EN: Wait for data after work, with timeout-only safety checks. Local stop outranks data; failures wake the motor coordinator.
    template <class Work>
    void start(Work work, HANDLE data = nullptr, HANDLE stopEvent = nullptr, HANDLE parent = nullptr,
               HANDLE service = nullptr, HANDLE failureWake = nullptr, DWORD guardMs = framewake::GuardMs)
    {
        auto fail = [this, failureWake](const char *event) {
            runtimediag::event(telemetry, "EXE.lane", strstr(event, "exception") ? "ERROR" : "WARN", event, "lane=%s",
                               name);
            failed = true;
            if (failureWake)
                SetEvent(failureWake);
        };
        if (!halt)
        {
            fail("stop_event_create_failed");
            return;
        }
        worker = std::thread([this, work, data, stopEvent, parent, service, guardMs, fail]() mutable {
            try
            {
                for (;;)
                {
                    const auto cancel = framewake::wait(stopEvent, parent, service, nullptr, 0, halt);
                    if (cancel == framewake::Wake::Stop)
                        return;
                    if (cancel != framewake::Wake::Timeout)
                    {
                        fail(cancel == framewake::Wake::Failed ? "data_wait_failed" : "dependency_process_ended");
                        return;
                    }
                    if (!work())
                    {
                        fail("work_rejected");
                        return;
                    }
                    const auto wake =
                        framewake::wait(stopEvent, parent, service, data, data ? guardMs : framewake::FallbackMs, halt);
                    if (wake == framewake::Wake::Stop)
                        return;
                    if (wake == framewake::Wake::Data)
                    {
                        ++dataWakes;
                        continue;
                    }
                    if (wake == framewake::Wake::Timeout)
                    {
                        ++timeoutChecks;
                        continue;
                    }
                    fail(wake == framewake::Wake::Failed ? "data_wait_failed" : "dependency_process_ended");
                    return;
                }
            }
            catch (...)
            {
                fail("work_exception");
            }
        });
    }

    void log() const
    {
        runtimediag::event(telemetry, "EXE.lane", "INFO", "data_wake_stats",
                           "lane=%s notifications=%llu timeout_checks=%llu", name, dataWakes.load(),
                           timeoutChecks.load());
    }

    void stop()
    {
        if (halt)
            SetEvent(halt);
        if (worker.joinable())
            worker.join();
    }

    ~Lane()
    {
        stop();
        if (halt)
            CloseHandle(halt);
    }
};

inline bool disconnected(DWORD error)
{
    return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_INVALID_HANDLE ||
           error == ERROR_NO_DATA;
}
}

// ZH: 电机目标、写入保护和独立看门狗。持有生命周期与设备身份校验，主助手退出后负责安全停止。
// EN: Motor targets, write guards and an independent watchdog. Validate lifecycle/device identity and stop safely after the main helper exits.

#include "bridge_ipc.h"
#include "jog_ring.h"
#include <array>
#include "haptic_cue.h"
#include <cfloat>
#include "ni_service_compatibility.h"

namespace ni24
{
constexpr DWORD Motion = 0x036a736d, Mode = 0x036a734d, BaseSpeed = 0x036a7373, Speed = 0x036a7353, True = 0x74727565;
constexpr DWORD RingMode =
    0x036a734c; // ZH: NI 灯环模式设置：零值立即发送关闭报告。 | EN: NI ring mode setter: zero sends an immediate off report.
constexpr DWORD Ticks = 0x036a7378, Property = 0x03567375, JogTension = 0x4a4f4754;

struct Target
{
    DWORD mode = 1;
    bool moving = false;
    DWORD deck = 0;
    float rpm = 45;
};

inline std::array<Target, 2> targets(const phase18::Frame &a, const phase18::Frame &b, DWORD pid, ULONGLONG created,
                                     ULONGLONG now)
{
    std::array<Target, 2> out{};
    if (!phase18::envelope(a, b, pid, created) || a.state != phase18::Active || !a.generation || a.tick > now ||
        now - a.tick > 100)
        return out;
    for (unsigned i = 0; i < 2; ++i)
    {
        DWORD f = a.facts[i];
        if (!(a.valid & (1u << i)) || !(f & phase18::Loaded) || (f & phase18::LineIn))
            continue;
        out[i].deck = a.deck[i];
        double rpm = 0;
        if (a.speedMode == 1 || a.speedMode == 2)
        {
            if (!std::isfinite(a.tempo[i]) || a.tempo[i] <= 0)
                continue;
            rpm = (a.speedMode == 1 ? 100.0 / 3.0 : 45.0) * a.tempo[i];
        }
        else if (a.speedMode == 3)
        {
            if (!std::isfinite(a.bpm[i]) || a.bpm[i] <= 0)
                continue;
            rpm = a.bpm[i] / 4.0;
        }
        else
            continue;
        if (!std::isfinite(rpm) || rpm <= 0 || rpm > 75.0)
            continue;
        out[i].rpm = static_cast<float>(rpm);
        if (out[i].rpm <= 0)
            continue;
        out[i].mode = (f & phase18::TT) ? 2 : 1;
        out[i].moving =
            (f & phase18::Playing) != 0 &&
            !(a.padding &
              (1u
               << (i +
                   4))); // ZH: 物理触摸由 NI 处理；触摸和松手之间保留反向状态。 | EN: NI owns physical touch; preserve reverse state across touch/release.
    }
    return out;
}

struct Controller
{
    std::array<Target, 2> previous{};
    bool known[2]{};

    template <class Send> bool update(unsigned i, const Target &requested, Send send)
    {
        Target next = requested;
        if (next.mode == 2 && (!std::isfinite(next.rpm) || next.rpm <= 0 || next.rpm > 75.0f))
        {
            next.mode = 1;
            next.moving = false;
            next.rpm = 45;
        }
        auto &old = previous[i];
        const bool reconfigure = !known[i] || old.mode != next.mode || old.deck != next.deck;
        // ZH: Motion 边沿会重置 NI 反转状态；只在已知的
        // EN: A Motion edge resets NI's backspin state. Preserve it only for a known,
        // ZH: 同 Deck 运行模式切换时保留；初始化、换 Deck 和停止都重置。
        // EN: same-deck live mode transition; initialization, deck changes and stops reset.
        const bool preserveMotion = known[i] && old.deck != 0 && old.deck == next.deck && old.moving && next.moving &&
                                    old.mode != next.mode && (old.mode == 1 || old.mode == 2) &&
                                    (next.mode == 1 || next.mode == 2);
        // ZH: 任一命令失败后设备可能已部分更新，下一轮必须完整重同步。
        // EN: A failed command may leave a partial device update; resynchronize fully on the next attempt.
        auto write = [&](DWORD op, DWORD side, DWORD value) {
            if (send(op, side, value))
                return true;
            known[i] = false;
            return false;
        };
        if (reconfigure)
        {
            if (!preserveMotion && !write(Motion, i, 0))
                return false;
            if (!write(Mode, i, next.mode))
                return false;
            if (next.mode == 2 || !known[i])
            {
                float rpm =
                    next.mode == 1 && (!std::isfinite(next.rpm) || next.rpm <= 0 || next.rpm > 75) ? 45.0f : next.rpm;
                DWORD bits = 0;
                memcpy(&bits, &rpm, 4);
                if (!write(BaseSpeed, i, bits) || !write(Speed, i, bits))
                    return false;
            }
            if (!preserveMotion && next.moving && !write(Motion, i, True))
                return false;
        }
        else if (old.moving != next.moving && !write(Motion, i, next.moving ? True : 0))
            return false;
        if (!reconfigure && next.mode == 2 && next.rpm != old.rpm)
        {
            DWORD bits = 0;
            memcpy(&bits, &next.rpm, 4);
            if (!write(BaseSpeed, i, bits) || !write(Speed, i, bits))
                return false;
        }
        old = next;
        known[i] = true;
        return true;
    }
};

struct HapticConfig
{
    bool known = false;
    DWORD ticks = 0, tension = 0;

    template <class Send> bool update(DWORD nextTicks, DWORD nextTension, Send send)
    {
        if (nextTicks > 2 || nextTension > 255)
            return false;
        if (!known || ticks != nextTicks)
        {
            if (!send(Ticks, 0, nextTicks * 12) || !send(Ticks, 1, nextTicks * 12))
            {
                known = false;
                return false;
            }
        }
        if (!known || tension != nextTension)
            if (!send(Property, JogTension, nextTension))
            {
                known = false;
                return false;
            }
        known = true;
        ticks = nextTicks;
        tension = nextTension;
        return true;
    }
};

struct Wire
{
    HANDLE pipe;
    DWORD words[3];
    DWORD error = 0;
};

inline DWORD WINAPI writer(void *raw)
{
    auto p = static_cast<Wire *>(raw);
    DWORD n = 0;
    if (!WriteFile(p->pipe, p->words, 12, &n, nullptr))
        p->error = GetLastError();
    else if (n != 12)
        p->error = ERROR_WRITE_FAULT;
    return 0;
}

inline bool send(HANDLE pipe, DWORD op, DWORD side, DWORD value)
{
    if (op == RingMode)
    {
        if (side > 1 || value != 0)
            return false;
    }
    else if (op == Property)
    {
        if (side != JogTension || value > 255)
            return false;
    }
    else if (side > 1 || (op != Motion && op != Mode && op != BaseSpeed && op != Speed && op != Ticks))
        return false;
    if (op == Ticks && value != 0 && value != 12 && value != 24)
        return false;
    Wire p{pipe, {op, side, value}, 0};
    HANDLE thread = CreateThread(nullptr, 0, writer, &p, 0, nullptr);
    if (!thread)
        return false;
    DWORD waited = WaitForSingleObject(thread, 200);
    if (waited != WAIT_OBJECT_0)
    {
        CancelSynchronousIo(thread);
        WaitForSingleObject(thread, INFINITE);
    }
    DWORD waitError = waited == WAIT_FAILED ? GetLastError() : 0;
    CloseHandle(thread);
    bool ok = waited == WAIT_OBJECT_0 && !p.error;
    if (!ok)
        SetLastError(waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : waitError ? waitError : p.error);
    return ok;
}

struct Shared
{
    DWORD magic = 0x4e493234, parent = 0, service = 0, padding = 0;
    ULONGLONG created = 0;
    uintptr_t server = 0, device = 0, vt = 0;
    BYTE focus[24]{};
    nicompat::Layout layout{};
    volatile LONG64 tick = 0;
    volatile LONG state =
        0; // ZH: 0 表示准备，1 表示活动，2 表示请求停止。 | EN: 0 preparing, 1 active, 2 stop requested
};

inline bool expired(LONG state, ULONGLONG tick, ULONGLONG now, bool parentEnded)
{
    return parentEnded || state == 2 || tick > now || now - tick > (state == 0 ? 5000u : 500u);
}

inline bool sameImage(HANDLE parent)
{
    wchar_t own[32768], other[32768];
    DWORD n = 32768;
    if (!GetModuleFileNameW(nullptr, own, 32768) || !QueryFullProcessImageNameW(parent, 0, other, &n))
        return false;
    return !_wcsicmp(own, other);
}

template <class T> bool remote(HANDLE h, uintptr_t at, T &v)
{
    SIZE_T n = 0;
    return ReadProcessMemory(h, reinterpret_cast<void *>(at), &v, sizeof(v), &n) && n == sizeof(v);
}

inline int watchdog(HANDLE pipe, HANDLE mapping, HANDLE ready, FILE *telemetry = nullptr)
{
    auto s = static_cast<Shared *>(MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Shared)));
    if (!s)
    {
        runtimediag::event(telemetry, "EXE.watchdog", "ERROR", "mapping_view_failed", "win_error=%lu", GetLastError());
        return 4;
    }
    BH parent(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, s->parent));
    BH ni(
        OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | SYNCHRONIZE,
                    FALSE, s->service));
    ULONG serverPid = 0;
    bool valid = s->magic == 0x4e493234 && parent.h && ni.h && sameImage(parent.h) &&
                 phase18::creation(parent.h) == s->created && GetNamedPipeServerProcessId(pipe, &serverPid) &&
                 serverPid == s->service &&
#ifdef NI24_WATCHDOG_TEST
                 s->service == s->parent && sameImage(ni.h);
#else
                 ([&]() {
                     auto location = nicompat::locate(ni.h, s->service);
                     return location && location.serverVtable == s->vt &&
                            nicompat::sameLayout(location.layout, s->layout);
                 })();
#endif
    auto identity = [&]() {
        uintptr_t vt = 0, dev = 0;
        BYTE focus[24]{};
        return WaitForSingleObject(ni.h, 0) == WAIT_TIMEOUT && remote(ni.h, s->server, vt) && vt == s->vt &&
               remote(ni.h, s->server + s->layout.serverDevice, dev) && dev == s->device &&
               remote(ni.h, s->server + s->layout.focus, focus) && !memcmp(focus, s->focus, 24);
    };
    int result = 4;
    if (valid && identity())
    {
        runtimediag::event(telemetry, "EXE.watchdog", "INFO", "ready", "helper_pid=%lu ni_pid=%lu", s->parent,
                           s->service);
        SetEvent(ready);
        while (!expired(InterlockedCompareExchange(&s->state, 0, 0),
                        static_cast<ULONGLONG>(InterlockedCompareExchange64(&s->tick, 0, 0)), GetTickCount64(),
                        WaitForSingleObject(parent.h, 0) != WAIT_TIMEOUT))
            Sleep(20);
        // ZH: 停止前保留实际状态，区分正常卸载、父进程退出、心跳超时和未来时间戳。
        // EN: Capture state before stopping to distinguish normal unload, parent exit, heartbeat expiry and future timestamps.
        const auto state = InterlockedCompareExchange(&s->state, 0, 0);
        const auto tick = static_cast<ULONGLONG>(InterlockedCompareExchange64(&s->tick, 0, 0));
        const auto now = GetTickCount64();
        const bool parentEnded = WaitForSingleObject(parent.h, 0) != WAIT_TIMEOUT;
        runtimediag::event(telemetry, "EXE.watchdog", "INFO", "stop_trigger",
                           "reason=%s state=%ld heartbeat_age_ms=%llu",
                           parentEnded  ? "helper_process_ended"
                           : state == 2 ? "stop_requested"
                           : tick > now ? "future_heartbeat"
                                        : "heartbeat_expired",
                           state, now >= tick ? now - tick : 0);
        InterlockedExchange(&s->state, 2);
        if (identity())
        {
            bool ok = true;
            // ZH: 先停止电机并明确熄灭灯环，再执行较慢的触觉清理。
            // EN: Stop motion first, then explicitly extinguish rings before slower haptic cleanup.
            for (DWORD i = 0; i < 2; ++i)
            {
                if (!identity() || !send(pipe, Motion, i, 0))
                    ok = false;
            }
            for (DWORD i = 0; i < 2; ++i)
            {
                if (!identity() || !send(pipe, RingMode, i, 0))
                    ok = false;
            }
            for (unsigned i = 0; i < 2; ++i)
            {
                hc90::Side empty;
                if (!identity() || !hc90::send(pipe, hc90::gatePacket(i, false)) ||
                    !hc90::send(pipe, hc90::listPacket(i, empty)) || !hc90::send(pipe, hc90::positionPacket(i, 0)))
                    ok = false;
            }
            // ZH: 回读运动状态字节，发出停止请求后才恢复中性 JOG 模式。
            // EN: Read back the motion bytes. Restore neutral JOG mode only after requesting stop.
            for (DWORD i = 0; i < 2; ++i)
            {
                if (!identity() || !send(pipe, Mode, i, 1))
                    ok = false;
            }
            bool off = false;
            for (unsigned retry = 0; retry < 10; ++retry)
            {
                BYTE left = 1, right = 1;
                off = identity() && remote(ni.h, s->device + s->layout.deviceJog + s->layout.motion[0], left) &&
                      remote(ni.h, s->device + s->layout.deviceJog + s->layout.motion[1], right) && !left && !right;
                if (off)
                    break;
                Sleep(20);
            }
            // ZH: 退出流程决定灯环最终状态，禁止恢复点亮的缓存。
            // EN: Shutdown owns the final ring state; never restore an illuminated cache.
            DWORD scale = 0;
            bool ringsOff =
                identity() && remote(ni.h, s->device + s->layout.deviceJog + s->layout.scale, scale) && scale == 2880;
            for (unsigned i = 0; i < 2; ++i)
            {
                ring18::Cache before{}, after{};
                if (!ringsOff || !identity() ||
                    !nicompat::cacheRead(ni.h, s->device + s->layout.deviceJog, s->layout, i, before) ||
                    !ring18::validCache(before))
                {
                    ringsOff = false;
                    continue;
                }
                before.mode = 0;
                if (!identity() || !nicompat::cacheWrite(ni.h, s->device + s->layout.deviceJog, s->layout, i, before) ||
                    !nicompat::cacheRead(ni.h, s->device + s->layout.deviceJog, s->layout, i, after) ||
                    memcmp(&before, &after, sizeof(before)))
                    ringsOff = false;
            }
            result = ok && off && ringsOff ? 0 : 5;
            runtimediag::event(telemetry, "EXE.watchdog", result ? "ERROR" : "INFO", "cleanup_verified",
                               "commands_ok=%u motors_off=%u rings_off=%u result=%d", ok ? 1u : 0u, off ? 1u : 0u,
                               ringsOff ? 1u : 0u, result);
        }
        else
            result =
                6; // ZH: 不得向替换后的设备或焦点所有者发送。 | EN: Never send to a replacement device/focus owner.
    }
    if (result == 4 || result == 6)
        runtimediag::event(telemetry, "EXE.watchdog", "WARN", "identity_rejected_no_output", "result=%d", result);
    UnmapViewOfFile(s);
    CloseHandle(pipe);
    CloseHandle(mapping);
    CloseHandle(ready);
    return result;
}

// ZH: 看门狗使用自己的父进程/内存校验；它不能因为参数来自助手就跳过身份检查。
// EN: The watchdog independently validates its parent and memory; helper-supplied arguments do not bypass identity checks.
inline int watchdogArgs(wchar_t **args, FILE *telemetry = nullptr)
{
    HANDLE h[3]{};
    for (unsigned i = 0; i < 3; ++i)
    {
        const wchar_t *p = args[i];
        if (!*p)
            return 4;
        for (auto q = p; *q; ++q)
            if (*q < L'0' || *q > L'9')
                return 4;
        wchar_t *end = nullptr;
        auto n = _wcstoui64(p, &end, 10);
        if (!n || *end)
            return 4;
        h[i] = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(n));
    }
    return watchdog(h[0], h[1], h[2], telemetry);
}

class Watch
{
    BH mapping, ready, process;
    Shared *shared = nullptr;

  public:
    Watch() = default;
    Watch(const Watch &) = delete;
    Watch &operator=(const Watch &) = delete;

    bool start(HANDLE pipe, DWORD service, uintptr_t server, uintptr_t device, uintptr_t vt, const BYTE *focus,
               const nicompat::Layout &layout)
    {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        mapping.h = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(Shared), nullptr);
        ready.h = CreateEventW(&sa, TRUE, FALSE, nullptr);
        if (!mapping.h || !ready.h)
            return false;
        shared = static_cast<Shared *>(MapViewOfFile(mapping.h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
        if (!shared)
            return false;
        new (shared) Shared{};
        shared->parent = GetCurrentProcessId();
        shared->created = phase18::creation(GetCurrentProcess());
        shared->service = service;
        shared->server = server;
        shared->device = device;
        shared->vt = vt;
        shared->layout = layout;
        memcpy(shared->focus, focus, 24);
        beat();
        BH inherited;
        if (!DuplicateHandle(GetCurrentProcess(), pipe, GetCurrentProcess(), &inherited.h, 0, TRUE,
                             DUPLICATE_SAME_ACCESS))
            return false;
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        std::vector<BYTE> data(bytes);
        auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(data.data());
        if (!InitializeProcThreadAttributeList(list, 1, 0, &bytes))
            return false;
        HANDLE handles[] = {inherited.h, mapping.h, ready.h};
        bool ok = UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles),
                                            nullptr, nullptr) != FALSE;
        wchar_t exe[32768];
        if (!GetModuleFileNameW(nullptr, exe, 32768))
            ok = false;
        std::wstring command = L"\"" + std::wstring(exe) + L"\" --ni-watchdog " +
                               std::to_wstring(reinterpret_cast<uintptr_t>(inherited.h)) + L" " +
                               std::to_wstring(reinterpret_cast<uintptr_t>(mapping.h)) + L" " +
                               std::to_wstring(reinterpret_cast<uintptr_t>(ready.h));
        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof(si);
        si.lpAttributeList = list;
        PROCESS_INFORMATION pi{};
        if (ok)
            ok = CreateProcessW(exe, command.data(), nullptr, nullptr, TRUE,
                                EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr, &si.StartupInfo,
                                &pi) != FALSE;
        DeleteProcThreadAttributeList(list);
        if (!ok)
            return false;
        process.h = pi.hProcess;
        CloseHandle(pi.hThread);
        HANDLE waits[] = {ready.h, process.h};
        return WaitForMultipleObjects(2, waits, FALSE, 4000) == WAIT_OBJECT_0;
    }

    void beat()
    {
        if (shared)
            InterlockedExchange64(&shared->tick, static_cast<LONG64>(GetTickCount64()));
    }

    void arm()
    {
        beat();
        if (shared)
            InterlockedExchange(&shared->state, 1);
    }

    bool alive()
    {
        return process.h && shared && InterlockedCompareExchange(&shared->state, 0, 0) < 2 &&
               WaitForSingleObject(process.h, 0) == WAIT_TIMEOUT;
    }

    DWORD finish()
    {
        if (!process.h)
            return 4;
        if (shared)
            InterlockedExchange(&shared->state, 2);
        if (WaitForSingleObject(process.h, 3000) != WAIT_OBJECT_0)
            return 7;
        DWORD code = 4;
        GetExitCodeProcess(process.h, &code);
        return code;
    }

    ~Watch()
    {
        finish();
        if (shared)
            UnmapViewOfFile(shared);
    }
};
}

// ZH: EXE 会话主控。验证 VDJ 发布租约和 NI 对象，分离电机、灯环、触觉通道，并按相反顺序停止和清理。
// EN: EXE session controller: validate the VDJ lease and NI objects, separate motor/ring/haptic lanes, then stop and clean up in reverse order.

#include <windows.h>
#include <tlhelp32.h>
#include <vector>
#include <atomic>
#include <algorithm>

inline bool bridgeVdj(HANDLE h)
{
    wchar_t path[32768]{};
    DWORD n = 32768;
    if (!QueryFullProcessImageNameW(h, 0, path, &n) || n >= 32768)
        return false;
    const auto leaf = wcsrchr(path, L'\\');
    return !_wcsicmp(leaf ? leaf + 1 : path, L"virtualdj.exe");
}

#include <memory>

class BridgeSession
{
  public:
    BH stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)}, thread;
    DWORD pid = 0;
    bool apply = false;
    DWORD durationMs = 0;
    ULONGLONG generation = 0;
    ULONGLONG created = 0;
    std::wstring sessionToken;
    const phase18::Frame *source = nullptr;
    FILE *telemetry = nullptr;
    std::atomic<LONG> status{0};
    std::atomic<LONG> frames{0};
    std::atomic<LONG> stale{0};

    ~BridgeSession()
    {
        if (thread.h)
            WaitForSingleObject(thread.h, INFINITE);
    }

    bool stopped()
    {
        return WaitForSingleObject(stop.h, 0) != WAIT_TIMEOUT;
    }

    template <class T> static bool read(HANDLE h, uintptr_t at, T &v)
    {
        SIZE_T n = 0;
        return ReadProcessMemory(h, reinterpret_cast<void *>(at), &v, sizeof(v), &n) && n == sizeof(v);
    }

    static bool bytes(HANDLE h, uintptr_t at, void *v, SIZE_T count)
    {
        SIZE_T n = 0;
        return ReadProcessMemory(h, reinterpret_cast<void *>(at), v, count, &n) && n == count;
    }

    static DWORD WINAPI entry(void *p)
    {
        auto s = static_cast<BridgeSession *>(p);
        try
        {
            s->status = s->execute();
        }
        catch (...)
        {
            s->status = E_UNEXPECTED;
        }
        return 0;
    }

    void note(const char *message, LONG value = 0)
    {
        runtimediag::event(telemetry, "EXE.session", value < 0 ? "ERROR" : "INFO", message,
                           "value=%ld generation=%llu vdj_pid=%lu", value, generation, pid);
    }

    sessiondiag::Exit diagnostic;

    // ZH: 会话总入口负责兜底退出记录；各工作通道另有 operation/side/opcode 上下文。
    // EN: The session entry records fallback exits; each lane maintains its own operation/side/opcode context.
    HRESULT execute()
    {
        diagnostic.file = telemetry;
        diagnostic.pid = pid;
        diagnostic.generation = generation;
        diagnostic.recorded = false;
        HRESULT result = executeBody();
        if (!diagnostic.recorded)
            diagnostic.fail("session_return", result);
        return result;
    }

    HRESULT executeBody()
    {
        if (durationMs > 60000 || !phase18::validToken(sessionToken.c_str()) || !source || !generation)
            return diagnostic.fail("invalid_session_arguments", E_INVALIDARG);
        note("validating_vdj");
        BH vdj(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | PROCESS_DUP_HANDLE, FALSE, pid));
        if (!vdj.h || !bridgeVdj(vdj.h) || phase18::creation(vdj.h) != created)
            return diagnostic.fail("vdj_identity_rejected", E_ACCESSDENIED);
        note("enabling_debug_privilege");
        BH token;
        TOKEN_PRIVILEGES tp{};
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &token.h))
            return diagnostic.fail("process_token_open_failed", E_ACCESSDENIED, 0, 0, GetLastError());
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (!LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &tp.Privileges[0].Luid))
            return diagnostic.fail("debug_privilege_lookup_failed", E_ACCESSDENIED, 0, 0, GetLastError());
        SetLastError(0);
        if (!AdjustTokenPrivileges(token.h, FALSE, &tp, 0, nullptr, nullptr) || GetLastError() != 0)
            return diagnostic.fail("debug_privilege_enable_failed", E_ACCESSDENIED, 0, 0, GetLastError());
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm)
            return diagnostic.fail("service_manager_open_failed", E_ACCESSDENIED, 0, 0, GetLastError());
        SC_HANDLE svc = OpenServiceW(scm, L"NIHardwareService", SERVICE_QUERY_STATUS);
        SERVICE_STATUS_PROCESS ss{};
        DWORD needed = 0;
        bool serviceOK =
            svc &&
            QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE *>(&ss), sizeof(ss), &needed) &&
            ss.dwCurrentState == SERVICE_RUNNING;
        if (svc)
            CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        if (!serviceOK)
            return diagnostic.fail("ni_service_unavailable");
        BH ni(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE |
                              (apply ? (PROCESS_VM_WRITE | PROCESS_VM_OPERATION) : 0),
                          FALSE, ss.dwProcessId));
        if (!ni.h)
            return diagnostic.fail("ni_process_open_failed", 3, 0, 0, GetLastError());
        note("validating_ni_service_identity");
        auto compatibility = nicompat::locate(ni.h, ss.dwProcessId);
        if (!compatibility)
        {
            nicompat::warn(telemetry, compatibility);
            return diagnostic.fail("ni_feature_location_rejected", 3);
        }
        nicompat::report(telemetry, compatibility);
        const auto &layout = compatibility.layout;
        note("scanning_service");
        uintptr_t vt = compatibility.serverVtable;
        std::vector<uintptr_t> found;
        std::vector<BYTE> buffer(1024 * 1024);
        ULONGLONG scanStart = GetTickCount64();
        const runtimetime::ScanBudget scanBudget{scanStart};
        uintptr_t address = 0;
        MEMORY_BASIC_INFORMATION mbi{};
        while (VirtualQueryEx(ni.h, reinterpret_cast<void *>(address), &mbi, sizeof(mbi)))
        {
            if (stopped() || WaitForSingleObject(vdj.h, 0) != WAIT_TIMEOUT)
                return 2;
            if (scanBudget.expired(GetTickCount64()))
                return diagnostic.fail("ni_object_scan_timeout", HRESULT_FROM_WIN32(ERROR_TIMEOUT),
                                       GetTickCount64() - scanStart, runtimetime::ObjectScanMs);
            uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = base + mbi.RegionSize;
            if (end <= address)
                break;
            if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
                (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_WRITECOPY ||
                 mbi.Protect == PAGE_EXECUTE_READWRITE || mbi.Protect == PAGE_EXECUTE_WRITECOPY))
            {
                for (uintptr_t at = base; at < end; at += buffer.size())
                {
                    if (stopped())
                        return 2;
                    if (scanBudget.expired(GetTickCount64()))
                        return diagnostic.fail("ni_object_scan_timeout", HRESULT_FROM_WIN32(ERROR_TIMEOUT),
                                               GetTickCount64() - scanStart, runtimetime::ObjectScanMs);
                    SIZE_T count = (std::min)(buffer.size(), static_cast<size_t>(end - at));
                    if (!bytes(ni.h, at, buffer.data(), count))
                        continue;
                    if (!runtimescan::alignedPointers(buffer.data(), count, vt, [&](size_t i) {
                            found.push_back(at + i);
                            return found.size() < 2;
                        }))
                        return diagnostic.fail("ni_device_count_rejected", 3, found.size(), 1);
                }
            }
            address = end;
        }
        if (found.size() != 1)
            return diagnostic.fail("ni_device_count_rejected", 3, found.size(), 1);
        note("unique_device_found");
        note("ni_object_scan_duration_ms", static_cast<LONG>(GetTickCount64() - scanStart));
        uintptr_t server = found[0], device = 0;
        if (!read(ni.h, server + layout.serverDevice, device) || !device)
            return diagnostic.fail("ni_device_pointer_unavailable", 3, device, 0, GetLastError());
        uintptr_t deviceVtable = 0;
        if (!read(ni.h, device, deviceVtable) || deviceVtable != compatibility.deviceVtable)
            return diagnostic.fail("ni_device_class_rejected", 3);
        uintptr_t jog = device + layout.deviceJog;
        BYTE focus[24]{};
        if (!bytes(ni.h, server + layout.focus, focus, 24))
            return diagnostic.fail("ni_focus_read_failed", 3, 0, 0, GetLastError());
        DWORD owner = 0, role = 0;
        memcpy(&owner, focus + 8, 4);
        memcpy(&role, focus + 16, 4);
        if (owner != 0x4e694d32 || role != 0x70726d79)
            return diagnostic.fail("ni_focus_owner_rejected", 3, owner, 0x4e694d32);
        // ZH: 两个可执行文件身份验证通过后，才能复制 VDJ 固定管道租约。
        // EN: Duplicate a pinned VDJ pipe lease only after both executable identities are verified.
        phase18::Frame start{}, again{};
        memcpy(&start, source, sizeof(start));
        MemoryBarrier();
        memcpy(&again, source, sizeof(again));
        bool initialCoherent = false;
        auto initial =
            phase18::decide(start, again, pid, created, GetTickCount64(), generation, false, initialCoherent);
        diagnostic.frame = start;
        if (initial != phase18::Action::Sync || !start.pipe)
            return diagnostic.fail("initial_publisher_snapshot_rejected", 3, start.sequence, again.sequence);
        BH pipe(duplicateNiLease(vdj.h, start.pipe, ss.dwProcessId));
        if (!pipe.h)
            return diagnostic.fail("ni_pipe_lease_duplicate_failed", 3, start.pipe, ss.dwProcessId, GetLastError());
        auto liveLease = [&](sessiondiag::Exit &report) {
            if (stopped())
                return report.reject("plugin_stop_requested");
            if (WaitForSingleObject(vdj.h, 0) != WAIT_TIMEOUT)
                return report.reject("vdj_process_ended");
            const auto lease = publisherread::readLease(source);
            ULONGLONG now = GetTickCount64();
            if (lease.magic != phase18::Magic || lease.version != phase18::Version || lease.pid != pid ||
                lease.created != created)
                return report.reject("publisher_lease_identity_changed", lease.pid, pid);
            if (lease.state != phase18::Active)
                return report.reject("publisher_lease_not_active", lease.state, phase18::Active);
            if (lease.generation != generation)
                return report.reject("publisher_generation_changed", lease.generation, generation);
            if (lease.pipe != start.pipe)
                return report.reject("publisher_pipe_changed", lease.pipe, start.pipe);
            if (!publisherread::matches(lease, pid, created, generation, start.pipe, now))
                return report.reject("publisher_lease_expired", now >= lease.tick ? now - lease.tick : 0, 250);
            return true;
        };
        if (!liveLease(diagnostic))
            return 3;
        framewake::Signals notifications;
        if (!notifications.connect(pid, sessionToken.c_str(), false))
            runtimediag::event(telemetry, "EXE.session", "WARN", "data_notifications_unavailable_fallback_50ms",
                               "win_error=%lu", GetLastError());
        else
            note("data_notifications_enabled");
        ni24::Watch motorWatch;
        ni24::Controller motors;
        ni24::HapticConfig haptics;
        hc90::Channel timelineChannel;
        if (!timelineChannel.open(pid, sessionToken.c_str()))
            return diagnostic.fail("haptic_channel_open_failed", 3, 0, 0, GetLastError());
        auto timeline = std::make_unique<hc90::Controller>();
        auto ha = std::make_unique<hc90::Frame>();
        auto hb = std::make_unique<hc90::Frame>();

        auto guard = [&](sessiondiag::Exit &report) {
            uintptr_t cur = 0, currentVtable = 0;
            DWORD scale = 0;
            BYTE f[24]{};
            if (!read(ni.h, server, currentVtable))
                return report.reject("ni_vtable_read_failed", 0, 0, GetLastError());
            if (currentVtable != vt)
                return report.reject("ni_vtable_changed", currentVtable, vt);
            uintptr_t deviceClass = 0;
            if (!read(ni.h, device, deviceClass) || deviceClass != compatibility.deviceVtable)
                return report.reject("ni_device_class_changed", deviceClass, compatibility.deviceVtable);
            if (WaitForSingleObject(ni.h, 0) != WAIT_TIMEOUT)
                return report.reject("ni_process_ended");
            if (!read(ni.h, server + layout.serverDevice, cur))
                return report.reject("ni_device_read_failed", 0, 0, GetLastError());
            if (cur != device)
                return report.reject("ni_device_changed", cur, device);
            if (!bytes(ni.h, server + layout.focus, f, 24))
                return report.reject("ni_focus_read_failed", 0, 0, GetLastError());
            if (memcmp(f, focus, 24))
                return report.reject("ni_focus_changed");
            if (!read(ni.h, jog + layout.scale, scale))
                return report.reject("ni_scale_read_failed", 0, 0, GetLastError());
            if (scale != 2880)
                return report.reject("ni_scale_rejected", scale, 2880);
            for (unsigned side = 0; side < 2; ++side)
            {
                BYTE motion = 0, enabled = 0;
                DWORD mode = 0;
                if (!read(ni.h, jog + layout.motion[side], motion))
                    return report.reject("ni_motion_read_failed", 0, 0, GetLastError(), side);
                if (motion > 1)
                    return report.reject("ni_motion_rejected", motion, 1, 0, side);
                if (!read(ni.h, jog + layout.mode[side], mode))
                    return report.reject("ni_mode_read_failed", 0, 0, GetLastError(), side);
                if (mode > 2)
                    return report.reject("ni_mode_rejected", mode, 2, 0, side);
                if (!read(ni.h, jog + layout.enabled[side], enabled))
                    return report.reject("ni_enabled_read_failed", 0, 0, GetLastError(), side);
                if (enabled != 1)
                    return report.reject("ni_enabled_rejected", enabled, 1, 0, side);
                ring18::Cache cache{};
                if (!nicompat::cacheRead(ni.h, jog, layout, side, cache))
                    return report.reject("ring_cache_read_failed", 0, 0, GetLastError(), side);
                if (!ring18::validCache(cache))
                    return report.reject("ring_cache_invalid", (ULONGLONG(cache.mode) << 32) | cache.angle, 2880, 0,
                                         side);
            }
            return true;
        };
        if (!guard(diagnostic))
            return 3;
        // ZH: 只有此写线程发送普通 NI 数据包；独立
        // EN: Only this writer thread sends ordinary NI packets. The independent
        // ZH: 看门狗持有应急句柄，验证失败即停止。
        // EN: watchdog holds an emergency handle and stops on validation failure.
        sessiondiag::Exit wireReport = diagnostic;
        wireReport.recorded = false;
        wireReport.context("pipe_dispatch");
        controlwork::Pipe commands(pipe.h, [&] {
            return liveLease(wireReport) && guard(wireReport) && motorWatch.alive();
        });
        std::function<bool()> targetCurrent;
        if (telemetry)
            runtimediag::section(telemetry, "NI SERVICE SESSION");
        auto sendMotorCommand = [&](DWORD op, DWORD side, DWORD value) {
            diagnostic.context("motor_command", side, op);
            DWORD words[] = {op, side, value};
            // ZH: 每条命令完成后，仅在当前发布者仍新鲜有效时
            // EN: Renew protection after each completed command only while the current
            // ZH: 续期保护，避免初始化过程饿死自身看门狗。
            // EN: publisher is fresh, preventing watchdog expiry during initialization.
            if (!controlwork::sendMotor(
                    commands, words, targetCurrent,
                    [&] {
                        return liveLease(diagnostic) && motorWatch.alive();
                    },
                    [&] {
                        motorWatch.beat();
                    }))
            {
                if (GetLastError() == ERROR_RETRY)
                    return false;
                return diagnostic.reject("motor_command_send_failed", value, 0, GetLastError());
            }
            if (telemetry)
            {
                runtimediag::event(telemetry, "EXE.motor", "INFO", "NI_COMMAND", "side=%lu opcode=0x%08lX value=%lu",
                                   side, op, value);
                fflush(telemetry);
            }
            return true;
        };
        if (apply && !motorWatch.start(pipe.h, ss.dwProcessId, server, device, vt, focus, layout))
        {
            note("ni_motor_watchdog_start_failed");
            nicompat::warn(telemetry, {0, "NI_watchdog_validation_or_start"});
            return 3;
        }
        if (apply)
        {
            for (DWORD side = 0; side < 2; ++side)
            {
                if (!liveLease(diagnostic) || !motorWatch.alive() || !sendMotorCommand(ni24::Motion, side, 0) ||
                    !sendMotorCommand(ni24::Mode, side, 1))
                    return 3;
                motorWatch.beat();
            }
            motorWatch.arm();
            note("ni_motor_watchdog_ready");
        }
        std::atomic<bool> motorReady{false};
        std::atomic<LONG> ringFrames{0}, hapticTimeouts{0};
        sessiondiag::Exit ringReport = diagnostic, hapticReport = diagnostic;
        ringReport.recorded = hapticReport.recorded = false;
        controlwork::Lane ringLane(telemetry, "ring"), hapticLane(telemetry, "haptic");
        if (apply)
        {
            ringLane.start(
                [&, announced = false]() mutable {
                    if (!announced)
                    {
                        note("ring_lane_tid", static_cast<LONG>(GetCurrentThreadId()));
                        announced = true;
                    }
                    ringReport.context("ring_lane");
                    if (stopped())
                        return true;
                    phase18::Frame a{}, b{};
                    memcpy(&a, source, sizeof(a));
                    MemoryBarrier();
                    memcpy(&b, source, sizeof(b));
                    auto now = GetTickCount64();
                    auto decision = publisherread::inspect(a, b, pid, created, now, generation, start.pipe);
                    if (decision.kind == publisherread::Kind::Skip)
                    {
                        notifications.requestRefresh();
                        return true;
                    }
                    if (decision.kind != publisherread::Kind::Run)
                        return ringReport.reject("ring_publisher_changed");
                    ringReport.frame = a;
                    if (decision.action != phase18::Action::Sync)
                        return true;
                    if (!liveLease(ringReport) || !guard(ringReport) || !motorWatch.alive())
                        return false;
                    for (unsigned side = 0; side < 2; ++side)
                    {
                        auto readCache = [&](unsigned i, ring18::Cache &cache) {
                            return nicompat::cacheRead(ni.h, jog, layout, i, cache);
                        };
                        auto writeCache = [&](unsigned i, const ring18::Cache &cache) {
                            return nicompat::cacheWrite(ni.h, jog, layout, i, cache);
                        };
                        if (!ring18::update(a, side, readCache, writeCache))
                            return ringReport.reject("ring_cache_update_failed", 0, 0, GetLastError(), side);
                    }
                    ++ringFrames;
                    return true;
                },
                notifications.get(framewake::Ring), stop.h, vdj.h, ni.h, notifications.get(framewake::Motor));
            hapticLane.start(
                [&, retryAt = ULONGLONG(0), lastTick = ULONGLONG(0), lastLog = ULONGLONG(0),
                 announced = false]() mutable {
                    if (!announced)
                    {
                        note("haptic_lane_tid", static_cast<LONG>(GetCurrentThreadId()));
                        announced = true;
                    }
                    if (stopped() || !motorReady || GetTickCount64() < retryAt)
                        return true;
                    hapticReport.context("haptic_lane");
                    phase18::Frame a{}, b{};
                    memcpy(&a, source, sizeof(a));
                    MemoryBarrier();
                    memcpy(&b, source, sizeof(b));
                    auto now = GetTickCount64();
                    auto decision = publisherread::inspect(a, b, pid, created, now, generation, start.pipe);
                    if (decision.kind == publisherread::Kind::Skip)
                    {
                        notifications.requestRefresh();
                        return true;
                    }
                    if (decision.kind != publisherread::Kind::Run)
                        return hapticReport.reject("haptic_publisher_changed");
                    if (decision.action != phase18::Action::Sync || now < a.tick || now - a.tick > 100)
                        return true;
                    hapticReport.frame = a;
                    if (!liveLease(hapticReport) || !guard(hapticReport) || !motorWatch.alive())
                        return false;
                    DWORD lastError = 0;
                    auto sendPacket = [&](const hc90::Packet &packet) {
                        DWORD op = 0, side = ~0u;
                        if (packet.bytes.size() >= 8)
                        {
                            memcpy(&op, packet.bytes.data(), 4);
                            memcpy(&side, packet.bytes.data() + 4, 4);
                        }
                        hapticReport.context("haptic_packet", side, op);
                        if (!liveLease(hapticReport))
                            return false;
                        bool ok = commands.send(packet.bytes.data(), packet.bytes.size(), false);
                        if (!ok)
                            lastError = GetLastError();
                        return ok;
                    };
                    auto config = [&](DWORD op, DWORD side, DWORD value) {
                        hapticReport.context("haptic_config", side, op);
                        DWORD words[] = {op, side, value};
                        bool ok = commands.send(words, sizeof(words), false);
                        if (!ok)
                            lastError = GetLastError();
                        return ok;
                    };
                    bool ok = haptics.update(a.ticks, a.tension, config);
                    bool captured = false, coherent = false;
                    if (ok)
                    {
                        coherent = hc90::capture(
                            *ha, *hb, pid, created, generation,
                            [&](hc90::Frame &x, hc90::Frame &y) {
                                captured = timelineChannel.snapshot(x, y);
                                return captured;
                            },
                            []() {
                                return GetTickCount64();
                            });
                        now = GetTickCount64();
                        if (coherent)
                            lastTick = ha->tick;
                        bool defer = captured && hc90::publishing(*ha, *hb) && lastTick && now >= lastTick &&
                                     now - lastTick <= 100;
                        if (defer)
                            notifications.requestRefresh();
                        if (!defer)
                            for (unsigned side = 0; side < 2; ++side)
                            {
                                hc90::Side empty;
                                const auto &wanted =
                                    coherent && ha->sides[side].deck == a.deck[side] ? ha->sides[side] : empty;
                                if (!timeline->update(side, wanted, sendPacket))
                                {
                                    ok = false;
                                    break;
                                }
                            }
                    }
                    if (!ok)
                    {
                        if (hapticReport.recorded || controlwork::disconnected(lastError) || lastError != ERROR_TIMEOUT)
                            return hapticReport.reject("haptic_lane_send_failed", 0, 0, lastError);
                        ++hapticTimeouts;
                        retryAt = GetTickCount64() + 250;
                        // ZH: 控制器只提交已完成的数据包；重试时从最后
                        // EN: Controller commits only packets that completed. Retry from its last
                        // ZH: 确认状态配合最新时间线继续，不排队重放旧位置。
                        // EN: confirmed state with the newest timeline, never queue old positions.
                        if (telemetry)
                        {
                            liveLogPrintf(
                                telemetry,
                                "tick=%llu HAPTIC_RETRY error=%lu side=%lu opcode=0x%08lX cooldown_ms=250 count=%ld\n",
                                GetTickCount64(), lastError, hapticReport.side, hapticReport.opcode,
                                hapticTimeouts.load());
                            fflush(telemetry);
                        }
                    }
                    if (GetTickCount64() - lastLog >= 30000)
                    {
                        lastLog = GetTickCount64();
                        note("haptic_lane_alive");
                    }
                    return true;
                },
                notifications.get(framewake::Haptic), stop.h, vdj.h, ni.h, notifications.get(framewake::Motor));
        }
        HRESULT result = 2;
        ULONGLONG end = durationMs ? GetTickCount64() + durationMs : 0, lastCoherent = GetTickCount64(),
                  lastLog = GetTickCount64();
        LONG skippedPublications = 0;
        ULONGLONG motorDataWakes = 0, motorTimeoutChecks = 0;
        status = 1;
        note("motor_lane_tid", static_cast<LONG>(GetCurrentThreadId()));
        note(apply ? "sync_started_independent_lanes" : "readonly_started");
        while ((!end || GetTickCount64() < end) && !stopped() && WaitForSingleObject(vdj.h, 0) == WAIT_TIMEOUT)
        {
            phase18::Frame a{}, b{};
            memcpy(&a, source, sizeof(a));
            MemoryBarrier();
            memcpy(&b, source, sizeof(b));
            ULONGLONG now = GetTickCount64();
            auto decision = publisherread::inspect(a, b, pid, created, now, generation, start.pipe);
            auto action = decision.action;
            diagnostic.context("motor_lane");
            if (decision.coherent)
            {
                diagnostic.frame = a;
                lastCoherent = now;
            }
            if (decision.kind == publisherread::Kind::Invalid)
            {
                result = diagnostic.fail("publisher_stable_frame_invalid");
                break;
            }
            if (action == phase18::Action::Stop || now - lastCoherent > 3000)
            {
                result = diagnostic.fail("publisher_stop_or_expired");
                break;
            }
            if (decision.kind == publisherread::Kind::Changed)
            {
                result = diagnostic.fail("publisher_generation_state_or_pipe_changed");
                break;
            }
            if (ringLane.failed || hapticLane.failed)
            {
                result = diagnostic.fail("control_lane_failed");
                break;
            }
            if (!guard(diagnostic))
            {
                result = 3;
                break;
            }
            if (apply && !motorWatch.alive())
            {
                result = diagnostic.fail("motor_watchdog_ended");
                break;
            }
            if (decision.kind == publisherread::Kind::Skip)
            {
                ++stale;
                ++skippedPublications;
                notifications.requestRefresh();
            }
            else if (apply)
            {
                motorWatch.beat();
                auto desired = ni24::targets(a, b, pid, created, now);
                for (unsigned i = 0; i < 2; ++i)
                {
                    const auto &old = motors.previous[i];
                    const auto &wanted = desired[i];
                    if (telemetry && (!motors.known[i] || old.mode != wanted.mode || old.moving != wanted.moving ||
                                      old.deck != wanted.deck || old.rpm != wanted.rpm))
                    {
                        runtimediag::event(telemetry, "EXE.motor", "INFO", "MOTOR_TARGET",
                                           "side=%u deck=%lu mode=%lu moving=%u rpm=%.3f", i, wanted.deck, wanted.mode,
                                           wanted.moving ? 1u : 0u, static_cast<double>(wanted.rpm));
                        fflush(telemetry);
                    }
                    targetCurrent = [&, wanted, i] {
                        phase18::Frame latest{}, again{};
                        memcpy(&latest, source, sizeof(latest));
                        MemoryBarrier();
                        memcpy(&again, source, sizeof(again));
                        auto checkNow = GetTickCount64();
                        if (!phase18::envelope(latest, again, pid, created) || latest.state != phase18::Active ||
                            latest.generation != generation || latest.pipe != start.pipe || latest.tick > checkNow ||
                            checkNow - latest.tick > 100)
                            return false;
                        const auto check = ni24::targets(latest, again, pid, created, checkNow)[i];
                        return check.deck == wanted.deck && check.mode == wanted.mode &&
                               check.moving == wanted.moving && check.rpm == wanted.rpm;
                    };
                    if (!motors.update(i, wanted, sendMotorCommand))
                    {
                        if (GetLastError() == ERROR_RETRY)
                        {
                            ++stale;
                            break;
                        }
                        result = diagnostic.fail("motor_target_update_failed", 3, 0, 0, GetLastError(), i);
                        break;
                    }
                }
                targetCurrent = {};
                if (result == 3)
                    break;
                motorReady = motors.known[0] && motors.known[1];
                if (action == phase18::Action::Sync)
                    ++frames;
                else
                    ++stale;
            }
            if (now - lastLog >= 30000)
            {
                note("heartbeat_valid_frames", frames.load());
                note("heartbeat_skipped_publications", skippedPublications);
                note("heartbeat_ring_frames", ringFrames.load());
                note("heartbeat_haptic_timeouts", hapticTimeouts.load());
                commands.log(telemetry);
                ringLane.log();
                hapticLane.log();
                lastLog = now;
            }
            // ZH: 新发布帧立即唤醒；无数据时仍定期验证租约、NI身份和看门狗，不重放历史命令。
            // EN: New frames wake immediately; timeout checks retain lease, NI identity and watchdog protection without replaying old commands.
            DWORD waitMs = notifications.get(framewake::Motor) ? framewake::GuardMs : framewake::FallbackMs;
            if (end)
            {
                const auto current = GetTickCount64();
                waitMs = (std::min)(waitMs, end > current ? static_cast<DWORD>(end - current) : DWORD(0));
            }
            const auto wake = framewake::wait(stop.h, vdj.h, ni.h, notifications.get(framewake::Motor), waitMs);
            if (wake == framewake::Wake::Failed)
            {
                result = diagnostic.fail("data_wait_failed", HRESULT_FROM_WIN32(GetLastError()));
                break;
            }
            if (wake == framewake::Wake::ParentEnded || wake == framewake::Wake::ServiceEnded)
            {
                result =
                    diagnostic.fail(wake == framewake::Wake::ParentEnded ? "vdj_process_ended" : "ni_process_ended");
                break;
            }
            if (wake == framewake::Wake::Stop)
                break;
            if (wake == framewake::Wake::Data)
                ++motorDataWakes;
            else
                ++motorTimeoutChecks;
        }
        if (!diagnostic.recorded)
            diagnostic.fail(stopped()                                        ? "plugin_stop_requested"
                            : WaitForSingleObject(vdj.h, 0) == WAIT_OBJECT_0 ? "vdj_process_ended"
                            : end && GetTickCount64() >= end                 ? "test_duration_complete"
                                                                             : "control_loop_ended",
                            result);
        // ZH: 看门狗接管退出前先合并所有生产线程；正在进行的普通
        // EN: Join all producers before the watchdog owns shutdown. In-flight normal
        // ZH: I/O 必须取消并等待完成，才能用同一连接执行应急清理。
        // EN: I/O is cancelled/joined before emergency cleanup uses the same connection.
        note("control_lanes_stopping");
        hapticLane.stop();
        ringLane.stop();
        commands.close();
        commands.log(telemetry);
        ringLane.log();
        hapticLane.log();
        runtimediag::event(telemetry, "EXE.session", "INFO", "data_wake_stats",
                           "lane=motor notifications=%llu timeout_checks=%llu", motorDataWakes, motorTimeoutChecks);
        note("control_lanes_joined");
        if (apply)
        {
            DWORD stoppedMotor = motorWatch.finish();
            note("ni_motor_watchdog_stop_result", static_cast<LONG>(stoppedMotor));
            if (stoppedMotor != 0 && stoppedMotor != 6)
                result = E_FAIL;
        }
        note("valid_frames", frames.load());
        note("skipped_publications", skippedPublications);
        note("ring_frames", ringFrames.load());
        note("haptic_timeouts", hapticTimeouts.load());
        return result;
    }
};
