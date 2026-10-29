// ZH: 共享基础工具：诊断日志、退出证据、计时与指针扫描；不承载设备会话。
// EN: Shared utilities: diagnostics, exit evidence, timing and pointer scanning; no device session ownership.
#pragma once

#include <cstdint>
#include <algorithm>
#include <windows.h>

namespace runtimetime
{
// ZH: 对象扫描预算独立于入口定位；每个内存块前检查，正常找到后立即返回。
// EN: Object-scan budget is separate from entry resolution; check before each block and return immediately on success.
inline constexpr uint64_t ObjectScanMs = 5000;

struct ScanBudget
{
    uint64_t start;

    bool expired(uint64_t now) const
    {
        return now < start || now - start >= ObjectScanMs;
    }
};

// ZH: 新代次立即重试，同代次连续失败从 250ms 退避到最多 2s，不引入固定启动延迟。
// EN: Retry new generations immediately; back off repeated same-generation failures from 250ms to 2s without delaying startup.
struct Retry
{
    uint64_t generation = 0, next = 0, delay = 0;

    bool ready(uint64_t value, uint64_t now) const
    {
        return value != generation || now >= next;
    }

    void failed(uint64_t value, uint64_t now, bool wasStable = false)
    {
        delay = value != generation || wasStable ? 250 : (std::min)(uint64_t(2000), delay ? delay * 2 : 250);
        generation = value;
        next = now + delay;
    }
};

// ZH: 使用高精度累计截止时间和余数，避免 17ms 整数节流漂移；落后时跳过旧帧，不补发突发流量。
// EN: Accumulate precise deadlines and remainders instead of rounding to 17ms; skip missed frames rather than burst to catch up.
struct Cadence
{
    uint64_t frequency, rate, next = 0, remainder = 0;
    bool initialized = false;

    explicit Cadence(uint64_t ticks, uint64_t hz = 60) : frequency(ticks), rate(hz)
    {
    }

    bool due(uint64_t now) const
    {
        return !initialized || now >= next;
    }

    void advance(uint64_t now)
    {
        const auto period = frequency / rate;
        if (!initialized || now > next + period)
        {
            next = now;
            remainder = 0;
            initialized = true;
        }
        next += period;
        remainder += frequency % rate;
        if (remainder >= rate)
        {
            ++next;
            remainder -= rate;
        }
    }

    uint32_t waitMs(uint64_t now) const
    {
        if (due(now))
            return 0;
        return static_cast<uint32_t>(((next - now) * 1000 + frequency - 1) / frequency);
    }
};

// ZH: DLL按10ms截止时间采集SDK状态；累计QPC截止时间避免“工作耗时+16ms”漂移，停止事件可立即中断。
// EN: Sample SDK state at 10ms deadlines in the DLL; QPC deadlines avoid work-plus-16ms drift and stop interrupts immediately.
inline constexpr uint64_t PublisherHz = 100;

class PublisherTimer
{
    HANDLE timer = nullptr;
    uint64_t frequency = 0;
    Cadence cadence{1, PublisherHz};

  public:
    PublisherTimer()
    {
        LARGE_INTEGER value{};
        if (!QueryPerformanceFrequency(&value) || value.QuadPart < static_cast<LONGLONG>(PublisherHz))
            return;
        frequency = static_cast<uint64_t>(value.QuadPart);
        cadence.frequency = frequency;
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer)
            timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }

    PublisherTimer(const PublisherTimer &) = delete;
    PublisherTimer &operator=(const PublisherTimer &) = delete;

    ~PublisherTimer()
    {
        if (timer)
        {
            CancelWaitableTimer(timer);
            CloseHandle(timer);
        }
    }

    bool valid() const
    {
        return timer && frequency;
    }

    void reset()
    {
        cadence.initialized = false;
    }

    DWORD wait(HANDLE stop)
    {
        LARGE_INTEGER now{};
        if (!valid() || !QueryPerformanceCounter(&now))
        {
            SetLastError(ERROR_INVALID_FUNCTION);
            return WAIT_FAILED;
        }
        const auto tick = static_cast<uint64_t>(now.QuadPart);
        cadence.advance(tick);
        const uint64_t remaining = cadence.next > tick ? cadence.next - tick : 1;
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>((remaining * 10000000 + frequency - 1) / frequency);
        if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
            return WAIT_FAILED;
        HANDLE waits[] = {stop, timer};
        return WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    }
};
}

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <emmintrin.h>

namespace runtimescan
{
static_assert(sizeof(uintptr_t) == 8, "The hardware runtime requires x64 pointers");

// ZH: 使用 x64 基线 SSE2 一次比较两个对齐指针；不要求 AVX，也不越过缓冲区尾部。
// EN: Compare two aligned pointers per iteration with baseline x64 SSE2; no AVX requirement or reads past the buffer end.
template <class Visitor>
bool alignedPointers(const unsigned char *bytes, size_t count, uintptr_t value, Visitor visitor)
{
    const auto wanted = _mm_set_epi32(static_cast<int>(value >> 32), static_cast<int>(value),
                                      static_cast<int>(value >> 32), static_cast<int>(value));
    size_t at = 0;
    for (; at + 16 <= count; at += 16)
    {
        const auto words = _mm_loadu_si128(reinterpret_cast<const __m128i *>(bytes + at));
        const auto mask = _mm_movemask_epi8(_mm_cmpeq_epi32(words, wanted));
        if ((mask & 255) == 255 && !visitor(at))
            return false;
        if ((mask & 65280) == 65280 && !visitor(at + 8))
            return false;
    }
    if (at + 8 <= count)
    {
        uintptr_t tail = 0;
        std::memcpy(&tail, bytes + at, 8);
        if (tail == value && !visitor(at))
            return false;
    }
    return true;
}
}

// ZH: 日志允许外部工具实时读取；文件访问共享不代表硬件锁，不能用日志文件协调线程。
// EN: Allow live external log readers; file sharing is not a hardware lock and must not coordinate worker threads.

#include <cstdio>
#include <share.h>
// ZH: 日志仅用于诊断，不作为同步原语；允许实时读取。
// EN: Logs are diagnostics, not synchronization primitives. Permit live readers.
#include <io.h>
#include <cstdarg>
#include <atomic>
#include <string>
#include <vector>

// ZH: 所有产品日志经过同一写入器；短文件锁协调助手/看门狗，锁忙时跳过日志而不阻塞控制。
// EN: All product records use one writer; a short file lock coordinates helper/watchdog, skipping busy records instead of blocking control.
inline FILE *openLiveLog(const wchar_t *path)
{
    FILE *f = _wfsopen(path, L"a+b", _SH_DENYNO);
    if (f)
        setvbuf(f, nullptr, _IONBF, 0);
    return f;
}

namespace livelog
{
inline constexpr ULONGLONG MaxBytes = 10ull * 1024 * 1024;
inline std::atomic<unsigned> skipped{0};
inline std::atomic<bool> historyChecked{false};

// ZH: 只删除最后一个主会话段头之前的完整历史；看门狗段头不建立新会话。
// EN: Remove only complete history before the last main-session heading; watchdog headings are not session boundaries.
inline bool readAt(HANDLE handle, ULONGLONG offset, char *buffer, DWORD size, DWORD &got)
{
    LARGE_INTEGER where{};
    where.QuadPart = static_cast<LONGLONG>(offset);
    return SetFilePointerEx(handle, where, nullptr, FILE_BEGIN) && ReadFile(handle, buffer, size, &got, nullptr);
}

inline ULONGLONG currentStart(HANDLE handle, ULONGLONG length)
{
    std::vector<char> block(65536);
    std::string carry;
    ULONGLONG last = 0;
    for (ULONGLONG offset = 0; offset < length;)
    {
        DWORD got = 0;
        DWORD size = static_cast<DWORD>((std::min)(ULONGLONG(block.size()), length - offset));
        if (!readAt(handle, offset, block.data(), size, got) || got != size)
            return 0;
        std::string text = carry + std::string(block.data(), got);
        ULONGLONG origin = offset - carry.size();
        for (const char *marker : {"\n-------------------- BRIDGE START --------------------\n",
                                   "\n-------------------- HELPER START --------------------\n"})
        {
            size_t at = text.rfind(marker);
            if (at != std::string::npos)
                last = (std::max)(last, origin + at);
        }
        carry = text.substr(text.size() > (size_t)128 ? text.size() - 128 : 0);
        offset += got;
    }
    return last;
}

inline bool removeHistory(HANDLE handle, ULONGLONG from, ULONGLONG length)
{
    std::vector<char> block(65536);
    ULONGLONG kept = length - from;
    for (ULONGLONG offset = 0; offset < kept;)
    {
        DWORD got = 0, written = 0;
        DWORD size = static_cast<DWORD>((std::min)(ULONGLONG(block.size()), kept - offset));
        if (!readAt(handle, from + offset, block.data(), size, got) || got != size)
            return false;
        LARGE_INTEGER where{};
        where.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(handle, where, nullptr, FILE_BEGIN) ||
            !WriteFile(handle, block.data(), got, &written, nullptr) || written != got)
            return false;
        offset += got;
    }
    LARGE_INTEGER end{};
    end.QuadPart = static_cast<LONGLONG>(kept);
    return SetFilePointerEx(handle, end, nullptr, FILE_BEGIN) && SetEndOfFile(handle);
}

inline int write(FILE *file, const char *text, size_t length, ULONGLONG limit = MaxBytes)
{
    const DWORD saved = GetLastError();
    if (!file)
    {
        SetLastError(saved);
        return -1;
    }
    _lock_file(file);
    HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file)));
    OVERLAPPED position{};
    if (handle == INVALID_HANDLE_VALUE ||
        !LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &position))
    {
        ++skipped;
        _unlock_file(file);
        SetLastError(saved);
        return -1;
    }
    fflush(file);
    LARGE_INTEGER size{};
    int result = -1;
    const unsigned lost = skipped.exchange(0);
    char notice[128]{};
    int noticeLength = lost ? sprintf_s(notice, "[WARN] LOG records_skipped=%u (concurrent writer)\n", lost) : 0;
    const bool begins = strstr(text, "-------------------- BRIDGE START --------------------") ||
                        strstr(text, "-------------------- HELPER START --------------------");
    if (GetFileSizeEx(handle, &size))
    {
        bool ready = true;
        ULONGLONG removed = 0;
        if (static_cast<ULONGLONG>(size.QuadPart) + length + noticeLength > limit && (begins || !historyChecked.load()))
        {
            // ZH: 新会话尚未写入时全文件都是历史；运行中只压缩当前主会话之前的内容。
            // EN: Before a new session starts all existing bytes are history; otherwise retain the whole current main session.
            try
            {
                removed = begins ? static_cast<ULONGLONG>(size.QuadPart)
                                 : currentStart(handle, static_cast<ULONGLONG>(size.QuadPart));
                if (removed)
                    ready = removeHistory(handle, removed, static_cast<ULONGLONG>(size.QuadPart));
            }
            catch (...)
            {
                removed = 0;
                ready = true;
            }
            historyChecked.store(ready);
        }
        if (ready && removed && _fseeki64(file, 0, SEEK_END) == 0)
            fputs("[INFO] LOG old session history removed; current session preserved\n", file);
        if (ready && _fseeki64(file, 0, SEEK_END) == 0)
        {
            if (noticeLength > 0)
                fwrite(notice, 1, static_cast<size_t>(noticeLength), file);
            result = static_cast<int>(fwrite(text, 1, length, file));
            fflush(file);
        }
    }
    if (begins)
        historyChecked.store(false);
    UnlockFileEx(handle, 0, 1, 0, &position);
    _unlock_file(file);
    SetLastError(saved);
    return result;
}
}

// ZH: 超 10MiB 只移除旧会话，当前会话可超限；保持同一文件对象以支持助手和看门狗长期句柄。
// EN: Above 10MiB remove old sessions only; the current session may exceed the threshold and persistent handles retain the same file object.
inline int liveLogPrintf(FILE *file, const char *format, ...)
{
    const DWORD saved = GetLastError();
    char record[8192]{};
    va_list args;
    va_start(args, format);
    int n = vsnprintf_s(record, sizeof(record), _TRUNCATE, format, args);
    va_end(args);
    const size_t length = n < 0 ? strlen(record) : static_cast<size_t>(n);
    int result = livelog::write(file, record, length);
    SetLastError(saved);
    return result;
}

namespace runtimediag
{
// ZH: 时间保留毫秒；日期、进程和时钟基准集中于段头，故障明细按需保留。
// EN: Keep millisecond time; session headings carry date/process/clock context, while failures retain relevant details.
inline void event(FILE *file, const char *component, const char *level, const char *name, const char *format = "", ...)
{
    const DWORD saved = GetLastError();
    if (file)
    {
        char detail[2048]{};
        va_list args;
        va_start(args, format);
        vsnprintf_s(detail, sizeof(detail), _TRUNCATE, format, args);
        va_end(args);
        SYSTEMTIME t{};
        GetLocalTime(&t);
        liveLogPrintf(file, "%02u:%02u:%02u.%03u %-5s %-12s %s%s%s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                      level, component, name, *detail ? " | " : "", detail);
    }
    SetLastError(saved);
}

inline void section(FILE *file, const char *title)
{
    const DWORD saved = GetLastError();
    SYSTEMTIME t{};
    GetLocalTime(&t);
    liveLogPrintf(
        file, "\n-------------------- %s --------------------\n%04u-%02u-%02u %02u:%02u:%02u | pid=%lu tick=%llu\n",
        title, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId(), GetTickCount64());
    SetLastError(saved);
}

inline void pathEvent(const std::wstring &path, const char *component, const char *level, const char *name,
                      DWORD value = 0)
{
    const DWORD saved = GetLastError();
    FILE *file = openLiveLog(path.c_str());
    if (file)
    {
        if (value)
            event(file, component, level, name, "value=%lu", value);
        else
            event(file, component, level, name);
        fclose(file);
    }
    SetLastError(saved);
}

// ZH: 兜底记录未标注的提前返回；正常结束时调用 finish，避免重复退出记录。
// EN: Record unclassified early returns; call finish on normal completion to avoid duplicate exit records.
struct Exit
{
    FILE *file;
    const char *component;
    bool finished = false;

    void finish(int result, const char *reason)
    {
        section(file, "EXIT");
        event(file, component, result ? "ERROR" : "INFO", "process_exit", "result=%d reason=%s", result, reason);
        finished = true;
    }

    ~Exit()
    {
        if (!finished)
            event(file, component, "ERROR", "process_exit", "reason=early_return_unclassified");
    }
};
}

// ZH: 每个会话或工作通道记录一次退出证据：操作名、侧别、指令、错误码及最后发布帧，防止后续错误覆盖首因。
// EN: Record exit evidence once per session/lane: operation, side, opcode, error and last frame, preserving the original cause.

#include "bridge_ipc.h"
#include <cstdio>

namespace sessiondiag
{
struct Exit
{
    FILE *file = nullptr;
    DWORD pid = 0;
    ULONGLONG generation = 0;
    phase18::Frame frame{};
    bool recorded = false;
    const char *operation = "startup";
    DWORD side = ~0u, opcode = 0;

    void context(const char *name, DWORD channel = ~0u, DWORD op = 0)
    {
        operation = name;
        side = channel;
        opcode = op;
    }

    HRESULT fail(const char *reason, HRESULT result = 3, ULONGLONG observed = 0, ULONGLONG expected = 0,
                 DWORD error = 0, DWORD failedSide = ~0u)
    {
        if (recorded)
            return result;
        recorded = true;
        if (!file)
            return result;
        ULONGLONG now = GetTickCount64();
        runtimediag::section(file, "SESSION EXIT");
        runtimediag::event(
            file, "EXE.session",
            FAILED(result) ? "ERROR"
            : result == 3  ? "WARN"
                           : "INFO",
            "SESSION_EXIT",
            "reason=%s operation=%s result=%ld win_error=%lu side=%lu opcode=0x%08lX observed=%llu expected=%llu",
            reason, operation, result, error, failedSide == ~0u ? side : failedSide, opcode, observed, expected);
        // ZH: 故障才展开快照；正常退出不重复打印整份状态。
        // EN: Expand snapshots only for failures; normal exits omit the repeated state dump.
        if (result)
        {
            liveLogPrintf(
                file,
                "  snapshot: generation=%llu frame_generation=%llu age_ms=%llu state=%lu valid=%lu speed_mode=%lu\n",
                generation, frame.generation, frame.tick && now >= frame.tick ? now - frame.tick : 0ull, frame.state,
                frame.valid, frame.speedMode);
            liveLogPrintf(
                file,
                "  left: deck_left=%lu facts_left=%lu bpm=%.3f tempo=%.4f | right: deck=%lu facts=%lu bpm=%.3f tempo=%.4f\n",
                frame.deck[0], frame.facts[0], frame.bpm[0], frame.tempo[0], frame.deck[1], frame.facts[1],
                frame.bpm[1], frame.tempo[1]);
        }
        return result;
    }

    bool reject(const char *reason, ULONGLONG observed = 0, ULONGLONG expected = 0, DWORD error = 0,
                DWORD failedSide = ~0u)
    {
        fail(reason, 3, observed, expected, error, failedSide);
        return false;
    }
};
}
