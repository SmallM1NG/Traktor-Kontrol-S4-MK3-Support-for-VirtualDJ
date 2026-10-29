// ZH: DLL与EXE之间的共享帧、会话身份、数据读取策略和NI管道租约。
// EN: DLL/helper IPC: shared frames, session identity, reader policy and NI pipe leases.
#pragma once

// ZH: DLL 到 EXE 的共享帧协议：序列号保护一致性，PID/创建时间/代次/心跳防止读取已失效会话。
// EN: DLL-to-EXE frame protocol: sequence numbers protect consistency; PID, creation time, generation and heartbeat reject stale sessions.

#include <windows.h>
#include <cwchar>
#include <cmath>
#include <cstring>
#include <cstdint>

namespace phase18
{
constexpr DWORD Magic = 0x53345048, Version = 9;

enum State : DWORD
{
    Waiting = 0,
    Active = 1,
    Stopping = 2
};

struct Frame
{
    DWORD magic, version, pid, sequence;
    ULONGLONG tick;
    double left, right;
    DWORD valid, state;
    ULONGLONG generation, created;
    ULONGLONG pipe = 0;
    DWORD facts[2]{};
    DWORD rgb[2]{};
    DWORD deck[2]{};
    DWORD
    speedMode = 2,
    padding =
        0; /* ZH: padding 位0..1为灯环模式，位2..3为有效触摸，位4..5为CUE预听；位6..31必须为0。 EN: padding bits0..1 encode ring mode, bits2..3 effective touch, bits4..5 CUE preview; bits6..31 must be zero. */
    double bpm[2]{};
    double tempo[2]{};
    DWORD ticks = 0, tension = 0;
};

static_assert(sizeof(Frame) == 144);

// ZH: 心跳和提交序号本身不代表业务数据变化；稳定暂停状态不重复唤醒消费者。
// EN: Heartbeats and commit sequences alone are not payload changes; stable paused state need not wake consumers repeatedly.
inline bool samePayload(Frame a, Frame b)
{
    a.sequence = b.sequence = 0;
    a.tick = b.tick = 0;
    return !memcmp(&a, &b, sizeof(a));
}

enum Fact : DWORD
{
    Loaded = 1,
    LineIn = 2,
    Audible = 4,
    NearEnd = 8,
    TT = 16,
    Playing = 32,
    Touched = 64
};
enum class Action
{
    Stop,
    Idle,
    Attach,
    Sync
};

inline bool envelope(const Frame &a, const Frame &b, DWORD pid, ULONGLONG created)
{
    return !memcmp(&a, &b, sizeof(a)) && !(a.sequence & 1) && a.magic == Magic && a.version == Version &&
           a.pid == pid && a.created == created && a.speedMode <= 3 && (a.padding & 3u) <= 2 && !(a.padding & ~63u) &&
           a.ticks <= 2 && a.tension <= 255 && a.state <= Stopping && !(a.valid & ~3u) && !(a.facts[0] & ~127u) &&
           !(a.facts[1] & ~127u) && a.rgb[0] <= 0xffffff && a.rgb[1] <= 0xffffff && a.deck[0] <= 4 && a.deck[1] <= 4 &&
           (!(a.valid & 1) || a.deck[0] >= 1) && (!(a.valid & 2) || a.deck[1] >= 1);
}

inline Action decide(const Frame &a, const Frame &b, DWORD pid, ULONGLONG created, ULONGLONG now, ULONGLONG attached,
                     bool ended, bool &coherent)
{
    coherent = envelope(a, b, pid, created);
    if (ended)
        return Action::Stop;
    if (!coherent)
        return Action::Idle;
    if (a.state == Stopping || a.tick > now || now - a.tick > 3000)
        return Action::Stop;
    if (a.state != Active || now - a.tick > 250 || !a.generation)
        return Action::Idle;
    return attached == a.generation ? Action::Sync : Action::Attach;
}

inline int angle(const Frame &f, unsigned side)
{
    double v = side ? f.right : f.left;
    return (f.valid & (1u << side)) && std::isfinite(v) && v >= 0 && v < 1 ? static_cast<int>(v * 2880) : -1;
}

inline ULONGLONG creation(HANDLE p)
{
    FILETIME c{}, e{}, k{}, u{};
    if (!GetProcessTimes(p, &c, &e, &k, &u))
        return 0;
    ULARGE_INTEGER v{};
    v.LowPart = c.dwLowDateTime;
    v.HighPart = c.dwHighDateTime;
    return v.QuadPart;
}

inline bool validToken(const wchar_t *token)
{
    if (!token || wcslen(token) != 32)
        return false;
    for (unsigned i = 0; i < 32; ++i)
        if (!((token[i] >= L'0' && token[i] <= L'9') || (token[i] >= L'a' && token[i] <= L'f')))
            return false;
    return true;
}

inline void name(wchar_t (&out)[128], const wchar_t *kind, DWORD pid, const wchar_t *token)
{
    swprintf_s(out, L"Local\\S4VDJ%sV9_%lu_%s", kind, pid, token);
}
}

// ZH: 发布者读策略：正在写入或短暂不一致的数据跳过本轮，只有租约、代次或稳定数据非法才终止会话。
// EN: Publisher read policy: skip in-flight or briefly inconsistent frames; terminate only on invalid leases, generations or stable data.

#include <cstddef>

namespace publisherread
{
enum class Kind
{
    Run,
    Skip,
    Invalid,
    Changed,
    Stop
};

struct Decision
{
    Kind kind = Kind::Invalid;
    phase18::Action action = phase18::Action::Idle;
    bool coherent = false;
};

inline Decision inspect(const phase18::Frame &a, const phase18::Frame &b, DWORD pid, ULONGLONG created, ULONGLONG now,
                        ULONGLONG generation, ULONGLONG pipe)
{
    // ZH: 发布进行中不代表身份校验失败；这对快照中的任何字段
    // EN: A publication in flight is not a failed identity check. No fields from
    // ZH: 都不能作为新的硬件目标使用。
    // EN: this pair may be consumed as a new hardware target.
    if ((a.sequence & 1) || (b.sequence & 1) || memcmp(&a, &b, sizeof(a)))
        return {Kind::Skip, phase18::Action::Idle, false};
    bool good = false;
    auto action = phase18::decide(a, b, pid, created, now, generation, false, good);
    if (!good)
        return {Kind::Invalid, action, false};
    if (action == phase18::Action::Stop)
        return {Kind::Stop, action, true};
    if (action == phase18::Action::Attach || a.state == phase18::Waiting || a.pipe != pipe)
        return {Kind::Changed, action, true};
    return {Kind::Run, action, true};
}

struct Lease
{
    DWORD magic, version, pid, state;
    ULONGLONG created, generation, pipe, tick;
};

// ZH: Windows x64 上这些自然对齐标量字段支持原子读取。
// EN: On Windows x64 these naturally aligned scalar fields are atomic loads.
// ZH: 正常发布会改变序列、事实与位置，但不会改变
// EN: Normal publishing changes sequence/facts/positions, but does not change the
// ZH: 租约身份；此项检查刻意不读取上述业务字段。
// EN: lease identity. This check deliberately never consumes those business fields.
// ZH: 对新发生变化的生命周期字段采取保守拒绝；硬件写入前
// EN: A newly changed lifecycle field is rejected conservatively; NI identity and
// ZH: 独立检查 NI 身份和父进程停止事件。
// EN: the parent's stop event are checked independently before hardware writes.
inline Lease readLease(const volatile phase18::Frame *source)
{
    Lease l{};
    l.magic = source->magic;
    l.version = source->version;
    l.pid = source->pid;
    l.created = source->created;
    l.state = source->state;
    l.generation = source->generation;
    l.pipe = source->pipe;
    l.tick = source->tick;
    MemoryBarrier();
    return l;
}

inline bool matches(const Lease &l, DWORD pid, ULONGLONG created, ULONGLONG generation, ULONGLONG pipe, ULONGLONG now)
{
    return l.magic == phase18::Magic && l.version == phase18::Version && l.pid == pid && l.created == created &&
           l.state == phase18::Active && l.generation == generation && l.pipe == pipe && l.tick <= now &&
           now - l.tick <= 250;
}

static_assert(offsetof(phase18::Frame, generation) % 8 == 0 && offsetof(phase18::Frame, pipe) % 8 == 0 &&
              offsetof(phase18::Frame, tick) % 8 == 0);

}

// ZH: 比较复制前后的内核管道对象身份；句柄整数相同不能证明连接仍有效。
// EN: Compare kernel pipe object identity across duplication; equal handle integers do not prove a live connection.

#include <windows.h>

inline bool samePipeObject(HANDLE a, HANDLE b)
{
    using Compare = BOOL(WINAPI *)(HANDLE, HANDLE);
    auto module = GetModuleHandleW(L"kernelbase.dll");
    auto compare = module ? reinterpret_cast<Compare>(GetProcAddress(module, "CompareObjectHandles")) : nullptr;
    return compare && compare(a, b);
}

// ZH: 复制 VDJ 已验证的 NI 管道租约；不另开硬件连接，防止和原生输出争抢设备。
// EN: Duplicate the validated VDJ NI pipe lease instead of opening another hardware connection that competes with native output.

#include <windows.h>

// ZH: 返回句柄归 EXE 所有；发布者在当前连接代次保持源租约打开。
// EN: Returned handle belongs to the EXE. The publisher keeps its source lease open for this generation.
inline HANDLE duplicateNiLease(HANDLE publisher, ULONGLONG raw, DWORD expectedServicePid)
{
    if (!raw || raw == ~ULONGLONG(0) || !expectedServicePid)
        return nullptr;
    HANDLE pipe = nullptr;
    if (!DuplicateHandle(publisher, reinterpret_cast<HANDLE>(raw), GetCurrentProcess(), &pipe, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
        return nullptr;
    DWORD flags = 0, pid = 0;
    if (GetFileType(pipe) != FILE_TYPE_PIPE || !GetNamedPipeInfo(pipe, &flags, nullptr, nullptr, nullptr) ||
        !(flags & PIPE_TYPE_MESSAGE) || (flags & PIPE_SERVER_END) || !GetNamedPipeServerProcessId(pipe, &pid) ||
        pid != expectedServicePid)
    {
        CloseHandle(pipe);
        return nullptr;
    }
    return pipe;
}

// ZH: 每个消费者独占一个自动复位通知，避免电机、灯环和触觉互相抢走唤醒；多次提交只保留最新帧。
// EN: Give each consumer its own auto-reset event; motor, ring and haptics cannot steal wakes, and bursts retain the latest frame.
namespace framewake
{
enum Channel : unsigned
{
    Motor = 0,
    Ring = 1,
    Haptic = 2,
    Count = 3
};

inline constexpr DWORD GuardMs = 100, FallbackMs = 50, DisconnectedMs = 250;
enum class Wake
{
    Data,
    Timeout,
    Stop,
    ParentEnded,
    ServiceEnded,
    Failed
};

class Signals
{
    HANDLE events[Count + 1]{};

  public:
    Signals() = default;
    Signals(const Signals &) = delete;
    Signals &operator=(const Signals &) = delete;

    ~Signals()
    {
        close();
    }

    void close()
    {
        const DWORD saved = GetLastError();
        for (auto &event : events)
        {
            if (event)
                CloseHandle(event);
            event = nullptr;
        }
        SetLastError(saved);
    }

    bool connect(DWORD pid, const wchar_t *token, bool producer)
    {
        close();
        if (!pid || !phase18::validToken(token))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return false;
        }
        const wchar_t *kinds[] = {L"MotorData", L"RingData", L"HapticData", L"RefreshData"};
        for (unsigned i = 0; i < Count + 1; ++i)
        {
            wchar_t name[128];
            phase18::name(name, kinds[i], pid, token);
            events[i] = producer ? CreateEventW(nullptr, FALSE, FALSE, name)
                                 : OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name);
            const DWORD error = GetLastError();
            if (!events[i] || (producer && error == ERROR_ALREADY_EXISTS))
            {
                SetLastError(error);
                close();
                return false;
            }
        }
        return true;
    }

    HANDLE get(Channel channel) const
    {
        return events[channel];
    }

    bool notify(Channel channel) const
    {
        return events[channel] && SetEvent(events[channel]);
    }

    bool phaseCommitted() const
    {
        const bool motor = notify(Motor), ring = notify(Ring);
        return motor && ring;
    }

    bool hapticCommitted() const
    {
        return notify(Haptic);
    }

    // ZH: 遇到正在提交的快照只请求下一次完整提交通知，不能自我SetEvent忙等，也不必等100ms安全检查。
    // EN: A torn snapshot requests a notification at the next complete commit; never self-signal a busy loop or wait for the 100ms guard.
    bool requestRefresh() const
    {
        return events[Count] && SetEvent(events[Count]);
    }

    bool takeRefresh() const
    {
        return events[Count] && WaitForSingleObject(events[Count], 0) == WAIT_OBJECT_0;
    }

    bool notifyAll() const
    {
        bool ok = true;
        for (unsigned i = 0; i < Count; ++i)
            if (!notify(static_cast<Channel>(i)))
                ok = false;
        return ok;
    }
};

// ZH: 停止/进程退出优先于数据；超时只用于安全检查，绝不在通知前清事件或循环忙等。
// EN: Stop/process exit outranks data. Timeouts perform safety checks; never clear events before waiting or busy-spin.
inline Wake wait(HANDLE stop, HANDLE parent, HANDLE service, HANDLE data, DWORD timeout, HANDLE localStop = nullptr)
{
    HANDLE handles[5]{};
    Wake kinds[5]{};
    DWORD count = 0;
    auto add = [&](HANDLE handle, Wake kind) {
        if (handle)
        {
            handles[count] = handle;
            kinds[count++] = kind;
        }
    };
    add(localStop, Wake::Stop);
    add(stop, Wake::Stop);
    add(parent, Wake::ParentEnded);
    add(service, Wake::ServiceEnded);
    add(data, Wake::Data);
    if (!count)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return Wake::Failed;
    }
    const DWORD result = WaitForMultipleObjects(count, handles, FALSE, timeout);
    if (result == WAIT_TIMEOUT)
        return Wake::Timeout;
    if (result < WAIT_OBJECT_0 + count)
        return kinds[result - WAIT_OBJECT_0];
    return Wake::Failed;
}
}
