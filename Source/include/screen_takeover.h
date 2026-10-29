// ZH: 可选屏幕发送接管：原生矩形挂接、双屏帧合并、编码发送线程和失效回退；不绘制XML。
// EN: Optional screen transport takeover: native rectangle hook, dual-screen frame merging, encoding/sender worker and failure fallback; no XML rendering.
#pragma once
#include "runtime_utils.h"

// ZH: 固定尺寸屏幕帧缓存与脏矩形合并；每侧保留最新状态，不无限排队，输出 NI 所需像素封包。
// EN: Fixed-size screen buffers and dirty-rectangle merging; retain the latest state per side without unbounded queues and build NI pixel packets.

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>

namespace screenframes
{
constexpr int Width = 320, Height = 240;

struct Rect
{
    int x = 0, y = 0, w = 0, h = 0;

    bool empty() const
    {
        return w == 0 || h == 0;
    }
};

inline Rect unite(Rect a, Rect b)
{
    if (a.empty())
        return b;
    if (b.empty())
        return a;
    int x = (std::min)(a.x, b.x), y = (std::min)(a.y, b.y);
    return {x, y, (std::max)(a.x + a.w, b.x + b.w) - x, (std::max)(a.y + a.h, b.y + b.h) - y};
}

inline bool valid(Rect r)
{
    return r.x >= 0 && r.y >= 0 && r.w > 0 && r.h > 0 && r.x <= Width - r.w && r.y <= Height - r.h;
}

struct Image
{
    std::array<uint32_t, Width * Height> pixels{};
    Rect dirty{};
    bool initialized = false;
    uint64_t revision = 0, oldestQpc = 0, latestQpc = 0;
};

struct Snapshot
{
    unsigned side = 0;
    Rect rect{};
    uint64_t revision = 0, oldestQpc = 0, latestQpc = 0;
    std::array<uint32_t, Width * Height> pixels{};
};

// ZH: 调用者只持有短时状态锁，此处禁止 I/O、分配和 GPU 访问。
// EN: Caller holds a short state lock. No I/O, allocation or GPU access here.
struct Latest
{
    std::array<Image, 2> images{};

    void reset()
    {
        for (auto &i : images)
        {
            i.initialized = false;
            i.dirty = {};
            i.revision = 0;
            i.oldestQpc = i.latestQpc = 0;
        }
    }

    template <class Copy>
    bool update(unsigned side, Rect rect, const uint32_t *source, int stride, int origin, Copy copy)
    {
        if (side > 1 || !valid(rect) || !source || stride != 640 || origin != static_cast<int>(side) * Width)
            return false;
        auto &i = images[side];
        Rect copied = i.initialized ? rect : Rect{0, 0, Width, Height};
        for (int y = copied.y; y < copied.y + copied.h; ++y)
            if (!copy(i.pixels.data() + y * Width + copied.x, source + y * stride + origin + copied.x,
                      static_cast<size_t>(copied.w) * 4))
            {
                i.initialized = false;
                i.dirty = {};
                return false;
            }
        i.initialized = true;
        i.dirty = unite(i.dirty, copied);
        ++i.revision;
        return true;
    }

    bool pending(unsigned side) const
    {
        return !images[side].dirty.empty();
    }

    bool take(unsigned side, Snapshot &out)
    {
        auto &i = images[side];
        if (!i.initialized || i.dirty.empty())
            return false;
        Rect r = i.dirty;
        int end = (r.x + r.w + 1) & ~1;
        r.x &= ~1;
        r.w = end - r.x;
        out.side = side;
        out.rect = r;
        out.revision = i.revision;
        out.oldestQpc = i.oldestQpc;
        out.latestQpc = i.latestQpc;
        for (int y = r.y; y < r.y + r.h; ++y)
            std::memcpy(out.pixels.data() + y * Width + r.x, i.pixels.data() + y * Width + r.x,
                        static_cast<size_t>(r.w) * 4);
        i.dirty = {};
        i.oldestQpc = i.latestQpc = 0;
        return true;
    }

    void retry(unsigned side, Rect r)
    {
        images[side].dirty = unite(images[side].dirty, r);
    }
};

inline void le32(uint8_t *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = static_cast<uint8_t>(n >> (8 * i));
}

inline void be16(uint8_t *p, unsigned n)
{
    p[0] = static_cast<uint8_t>(n >> 8);
    p[1] = static_cast<uint8_t>(n);
}

inline bool packet(const Snapshot &s, std::vector<uint8_t> &out)
{
    auto r = s.rect;
    if (s.side > 1 || !valid(r) || (r.x & 1) || (r.w & 1))
        return false;
    size_t bytes = static_cast<size_t>(r.w) * r.h * 2;
    out.resize(bytes + 48);
    std::fill(out.begin(), out.begin() + 40, uint8_t(0));
    le32(out.data(), 0x03647344);
    le32(out.data() + 4, s.side);
    le32(out.data() + 12, 0x01e00110);
    le32(out.data() + 16, static_cast<uint32_t>(bytes + 28));
    out[20] = 0x84;
    out[22] = static_cast<uint8_t>(s.side);
    out[23] = 0x60;
    be16(out.data() + 28, r.x);
    be16(out.data() + 30, r.y);
    be16(out.data() + 32, r.w);
    be16(out.data() + 34, r.h);
    unsigned words = static_cast<unsigned>(bytes / 4);
    out[36] = static_cast<uint8_t>(words >> 24);
    out[37] = static_cast<uint8_t>(words >> 16);
    out[38] = static_cast<uint8_t>(words >> 8);
    out[39] = static_cast<uint8_t>(words);
    size_t at = 40;
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x)
        {
            uint32_t c = s.pixels[y * Width + x];
            unsigned v = ((c >> 19) & 31u) << 11 | ((c >> 10) & 63u) << 5 | ((c >> 3) & 31u);
            be16(out.data() + at, v);
            at += 2;
        }
    le32(out.data() + at, 3);
    out[at + 4] = 0x40;
    out[at + 5] = 0;
    out[at + 6] = static_cast<uint8_t>(s.side);
    out[at + 7] = 0;
    return true;
}
}

// ZH: 独立屏幕发送线程：合并最新帧、左右轮流发送、统计排队和写入延迟；失败撤销租约并交回原生重同步。
// EN: Independent screen sender: merge latest frames, alternate sides and measure queue/write latency; revoke failed leases and request native resync.

#include <windows.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <cstdio>
#include <string>

#include "runtime_utils.h"

namespace screenasync
{
struct Lease
{
    HANDLE pipe = nullptr;
    ULONG_PTR owner = 0;
    std::atomic<bool> live{true}, fault{false};

    ~Lease()
    {
        if (pipe)
            CloseHandle(pipe);
    }
};

using Validate = bool (*)(const Lease &);
enum class Submit
{
    Queued,
    Native,
    Resync
};

__declspec(noinline) inline bool copyPixels(void *dest, const void *source, size_t size)
{
    __try
    {
        std::memcpy(dest, source, size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

struct Timing
{
    static constexpr unsigned bounds[13] = {1, 2, 5, 10, 16, 20, 33, 50, 66, 100, 200, 500, 1000};
    ULONGLONG count = 0, sumUs = 0, maxUs = 0;
    std::array<ULONGLONG, 14> buckets{};

    void add(ULONGLONG us)
    {
        ++count;
        sumUs += us;
        maxUs = (std::max)(maxUs, us);
        unsigned i = 0;
        while (i < 13 && us > static_cast<ULONGLONG>(bounds[i]) * 1000)
            ++i;
        ++buckets[i];
    }

    unsigned p95UpperMs() const
    {
        if (!count)
            return 0;
        ULONGLONG target = (count * 95 + 99) / 100, seen = 0;
        for (unsigned i = 0; i < 14; ++i)
        {
            seen += buckets[i];
            if (seen >= target)
                return i < 13 ? bounds[i] : 0;
        }
        return 0;
    }
};

class Pump
{
    std::mutex lock, writeLock;
    screenframes::Latest frames;
    std::shared_ptr<Lease> lease;
    HANDLE wake = nullptr, stopEvent = nullptr, thread = nullptr;
    Validate validate;
    std::wstring logPath;
    std::atomic<bool> accepting{false};
    LARGE_INTEGER qpcFrequency{};
    Timing writeTiming[2], latestTiming[2], oldestTiming[2], queueTiming[2];
    ULONGLONG sideSent[2]{}, sideBytes[2]{}, priorSent[2]{}, priorBytes[2]{}, lastMetricTick = 0;
    std::atomic<ULONGLONG> accepted{0}, merged{0}, sent{0}, bytes{0}, errors{0}, maxSubmitUs{0};

    static DWORD WINAPI entry(void *p)
    {
        static_cast<Pump *>(p)->run();
        return 0;
    }

    void report(const char *event, DWORD code = 0)
    {
        if (logPath.empty())
            return;
        FILE *f = openLiveLog(logPath.c_str());
        if (!f)
            return;
        runtimediag::event(f, "DLL.screen", code ? "WARN" : "INFO", event, code ? "error=%lu" : "", code);
        fclose(f);
        if (!strcmp(event, "stats") || !strcmp(event, "worker_stopped"))
        {
            f = openLiveLog(logPath.c_str());
            if (!f)
                return;
            ULONGLONG tick = GetTickCount64(), span = tick - lastMetricTick;
            for (unsigned side = 0; side < 2; ++side)
            {
                auto &w = writeTiming[side];
                auto &l = latestTiming[side];
                auto &o = oldestTiming[side];
                auto &q = queueTiming[side];
                runtimediag::event(f, "DLL.screen", "INFO", "screen_async_timing",
                                   "side=%u span_ms=%llu frames=%llu bytes=%llu errors=%llu", side, span,
                                   sideSent[side] - priorSent[side], sideBytes[side] - priorBytes[side], errors.load());
                liveLogPrintf(
                    f,
                    "  lifetime us avg/max (p95 upper ms): write=%llu/%llu (%u) latest=%llu/%llu (%u) oldest=%llu/%llu (%u) queue=%llu/%llu (%u)\n",
                    w.count ? w.sumUs / w.count : 0, w.maxUs, w.p95UpperMs(), l.count ? l.sumUs / l.count : 0, l.maxUs,
                    l.p95UpperMs(), o.count ? o.sumUs / o.count : 0, o.maxUs, o.p95UpperMs(),
                    q.count ? q.sumUs / q.count : 0, q.maxUs, q.p95UpperMs());
                priorSent[side] = sideSent[side];
                priorBytes[side] = sideBytes[side];
            }
            lastMetricTick = tick;
            fclose(f);
        }
    }

    ULONGLONG micros(LONGLONG ticks) const
    {
        return ticks > 0 ? static_cast<ULONGLONG>(ticks * 1000000 / qpcFrequency.QuadPart) : 0;
    }

    // ZH: 独立发送循环不调用 VDJ SDK，只消费 CPU 像素；统计不增加发送频率。
    // EN: The sender consumes CPU pixels without VDJ SDK calls; statistics do not increase the send rate.
    void run()
    {
        report("worker_ready");
        auto snapshot = std::make_unique<screenframes::Snapshot>();
        std::vector<uint8_t> data;
        data.reserve(153648);
        unsigned turn = 0;
        ULONGLONG lastLog = GetTickCount64();
        HANDLE waits[] = {stopEvent, wake};
        const auto frequency = static_cast<uint64_t>(qpcFrequency.QuadPart);
        runtimetime::Cadence cadence[2]{runtimetime::Cadence(frequency), runtimetime::Cadence(frequency)};
        std::weak_ptr<Lease> previous;
        for (;;)
        {
            if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0)
                break;
            for (unsigned n = 0; n < 2; ++n)
            {
                unsigned side = (turn + n) % 2;
                std::shared_ptr<Lease> job;
                {
                    std::lock_guard<std::mutex> guard(lock);
                    if (previous.lock() != lease)
                    {
                        for (auto &clock : cadence)
                            clock = runtimetime::Cadence(frequency);
                        previous = lease;
                    }
                    LARGE_INTEGER now{};
                    QueryPerformanceCounter(&now);
                    if (!lease || !lease->live.load() || lease->fault.load() ||
                        !cadence[side].due(static_cast<uint64_t>(now.QuadPart)) || !frames.take(side, *snapshot))
                        continue;
                    job = lease;
                }
                if (!screenframes::packet(*snapshot, data))
                    continue;
                DWORD written = 0, error = 0;
                bool ok = false, attempted = false;
                LARGE_INTEGER sendBegin{}, sendEnd{};
                {
                    std::lock_guard<std::mutex> guard(writeLock);
                    if (WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0 && job->live.load() && !job->fault.load() &&
                        validate && validate(*job))
                    {
                        attempted = true;
                        QueryPerformanceCounter(&sendBegin);
                        ok = WriteFile(job->pipe, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                             written == data.size();
                        if (!ok)
                            error = GetLastError();
                        QueryPerformanceCounter(&sendEnd);
                    }
                }
                if (attempted && ok)
                {
                    ++sent;
                    bytes += written;
                    ++sideSent[side];
                    sideBytes[side] += written;
                    cadence[side].advance(static_cast<uint64_t>(sendBegin.QuadPart));
                    writeTiming[side].add(micros(sendEnd.QuadPart - sendBegin.QuadPart));
                    if (snapshot->latestQpc)
                    {
                        latestTiming[side].add(micros(sendEnd.QuadPart - static_cast<LONGLONG>(snapshot->latestQpc)));
                        queueTiming[side].add(micros(sendBegin.QuadPart - static_cast<LONGLONG>(snapshot->latestQpc)));
                    }
                    if (snapshot->oldestQpc)
                        oldestTiming[side].add(micros(sendEnd.QuadPart - static_cast<LONGLONG>(snapshot->oldestQpc)));
                }
                else if (attempted && job->live.load() && WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0)
                {
                    job->fault.store(true);
                    ++errors;
                    report("write_failed_native_resync", error);
                }
                else if (!attempted && job->live.load() && WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0)
                {
                    job->fault.store(true);
                    ++errors;
                    report("lease_rejected_native_resync");
                }
            }
            turn = (turn + 1) % 2;
            if (GetTickCount64() - lastLog >= 5000)
            {
                report("stats");
                lastLog = GetTickCount64();
            }
            // ZH: 等待新帧或最近截止时间，无帧时仅为统计唤醒；合并中的更新不重复发事件。
            // EN: Wait for a new frame or the nearest deadline; idle wakeups serve statistics only, and merged updates do not signal repeatedly.
            DWORD waitMs = static_cast<DWORD>(5000 - (std::min)(ULONGLONG(5000), GetTickCount64() - lastLog));
            {
                std::lock_guard<std::mutex> guard(lock);
                LARGE_INTEGER now{};
                QueryPerformanceCounter(&now);
                if (lease && lease->live.load() && !lease->fault.load())
                    for (unsigned side = 0; side < 2; ++side)
                        if (frames.pending(side))
                            waitMs =
                                (std::min)(waitMs, (std::max)(DWORD(1), static_cast<DWORD>(cadence[side].waitMs(
                                                                            static_cast<uint64_t>(now.QuadPart)))));
            }
            DWORD result = WaitForMultipleObjects(2, waits, FALSE, waitMs);
            if (result == WAIT_OBJECT_0 || result == WAIT_FAILED)
                break;
        }
        report("worker_stopped");
    }

  public:
    explicit Pump(Validate v) : validate(v)
    {
    }

    bool start(const std::wstring &path = {})
    {
        if (thread)
            return false;
        logPath = path;
        if (!QueryPerformanceFrequency(&qpcFrequency) || qpcFrequency.QuadPart < 60)
        {
            report("clock_initialize_failed", ERROR_NOT_SUPPORTED);
            return false;
        }
        lastMetricTick = GetTickCount64();
        wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!wake || !stopEvent)
        {
            if (wake)
                CloseHandle(wake);
            if (stopEvent)
                CloseHandle(stopEvent);
            wake = stopEvent = nullptr;
            return false;
        }
        accepting.store(true);
        thread = CreateThread(nullptr, 0, entry, this, 0, nullptr);
        if (!thread)
        {
            accepting.store(false);
            CloseHandle(wake);
            CloseHandle(stopEvent);
            wake = stopEvent = nullptr;
            return false;
        }
        return true;
    }

    bool bind(ULONG_PTR owner, HANDLE pipe)
    {
        HANDLE duplicate = nullptr;
        if (!accepting.load() || !DuplicateHandle(GetCurrentProcess(), pipe, GetCurrentProcess(), &duplicate, 0, FALSE,
                                                  DUPLICATE_SAME_ACCESS))
            return false;
        auto fresh = std::make_shared<Lease>();
        fresh->owner = owner;
        fresh->pipe = duplicate;
        {
            std::lock_guard<std::mutex> guard(lock);
            if (lease)
                lease->live.store(false);
            lease = fresh;
            frames.reset();
        }
        report("connection_bound");
        return true;
    }

    void unbind()
    {
        {
            std::lock_guard<std::mutex> guard(lock);
            if (lease)
                lease->live.store(false);
            lease.reset();
            frames.reset();
        }
        if (thread)
            CancelSynchronousIo(thread);
    }

    // ZH: 绘制线程只合并最新画面并唤醒消费者，不在此等待管道发送。
    // EN: The draw thread merges the newest image and wakes the consumer without waiting for pipe writes.
    Submit submit(ULONG_PTR owner, const uint32_t *pixels, screenframes::Rect r, unsigned side, int origin, int stride)
    {
        if (!accepting.load())
            return Submit::Native;
        LARGE_INTEGER begin{}, end{};
        QueryPerformanceCounter(&begin);
        Submit result = Submit::Native;
        bool notify = false;
        {
            std::lock_guard<std::mutex> guard(lock);
            if (lease && lease->owner == owner && lease->live.load())
            {
                if (lease->fault.load())
                    return Submit::Resync;
                bool pending = side < 2 && frames.pending(side);
                if (frames.update(side, r, pixels, stride, origin, copyPixels))
                {
                    if (!pending)
                        frames.images[side].oldestQpc = static_cast<uint64_t>(begin.QuadPart);
                    frames.images[side].latestQpc = static_cast<uint64_t>(begin.QuadPart);
                    ++accepted;
                    if (pending)
                        ++merged;
                    notify = !pending;
                    result = Submit::Queued;
                }
            }
        }
        QueryPerformanceCounter(&end);
        ULONGLONG us = static_cast<ULONGLONG>((end.QuadPart - begin.QuadPart) * 1000000 / qpcFrequency.QuadPart),
                  old = maxSubmitUs.load();
        while (old < us && !maxSubmitUs.compare_exchange_weak(old, us))
        {
        }
        if (notify)
            SetEvent(wake);
        return result;
    }

    // ZH: 原生回退只共享发送写锁，正常提交不获取该锁。
    // EN: Native fallback shares only the sender's write lock. Normal submit never takes it.
    template <class F> bool native(F f)
    {
        std::lock_guard<std::mutex> guard(writeLock);
        return f();
    }

    bool stop()
    {
        accepting.store(false);
        if (!thread)
            return true;
        unbind();
        SetEvent(stopEvent);
        SetEvent(wake);
        for (unsigned i = 0; i < 30; ++i)
        {
            CancelSynchronousIo(thread);
            if (WaitForSingleObject(thread, 100) == WAIT_OBJECT_0)
            {
                CloseHandle(thread);
                CloseHandle(wake);
                CloseHandle(stopEvent);
                thread = wake = stopEvent = nullptr;
                return true;
            }
        }
        report("stop_timeout_storage_retained");
        return false;
    }

    ULONGLONG sentCount() const
    {
        return sent.load();
    }

    ULONGLONG errorCount() const
    {
        return errors.load();
    }
};
}

// ZH: 屏幕绘制入口适配层。回调只提交像素，队列接管失效时退回原生发送，不改变 XML 内容。
// EN: Screen draw adapter: callbacks submit pixels only; invalid takeover falls back to native sending without changing XML content.

#include "bridge_ipc.h"
#include "vdj_compatibility.h"
#include <MinHook.h>

namespace screenasync
{
using NativeRect = bool(__fastcall *)(void *, const uint32_t *, int, int, int, int, unsigned char, int, int);
inline NativeRect original = nullptr;
inline void *target = nullptr;
inline std::atomic<bool> enabled{false};

inline bool nativeLease(const Lease &l)
{
    ULONG_PTR raw = 0, vt = 0;
    DWORD type = 0, width = 0, height = 0;
    SIZE_T got = 0;
    auto read = [&](ULONG_PTR addr, void *out, size_t size) {
        return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(addr), out, size, &got) &&
               got == size;
    };
    return l.live.load() && read(l.owner, &vt, sizeof(vt)) && vt == vdjcompat::addresses.displayVtable &&
           read(l.owner + vdjcompat::addresses.displayType, &type, sizeof(type)) && type == 11 &&
           read(l.owner + vdjcompat::addresses.displayWidth, &width, sizeof(width)) && width == 640 &&
           read(l.owner + vdjcompat::addresses.displayHeight, &height, sizeof(height)) && height == 240 &&
           read(l.owner + vdjcompat::addresses.displayPipe, &raw, sizeof(raw)) && raw &&
           samePipeObject(reinterpret_cast<HANDLE>(raw), l.pipe);
}

inline Pump pump(nativeLease);
inline std::mutex modeLock;
inline bool takeover = true;
inline unsigned nativeResync = 3;

inline bool __fastcall rect(void *owner, const uint32_t *pixels, int x, int y, int w, int h, unsigned char side,
                            int origin, int stride)
{
    std::lock_guard<std::mutex> modeGuard(modeLock);
    if (enabled.load() && takeover)
    {
        auto result = pump.submit(reinterpret_cast<ULONG_PTR>(owner), pixels, {x, y, w, h}, side, origin, stride);
        if (result == Submit::Queued)
            return true;
        return pump.native([&]() {
            return result == Submit::Resync && side < 2 && stride == 640 && origin == side * 320
                       ? original(owner, pixels, 0, 0, 320, 240, side, origin, stride)
                       : original(owner, pixels, x, y, w, h, side, origin, stride);
        });
    }
    return pump.native([&]() {
        bool full = side < 2 && stride == 640 && origin == side * 320 && (nativeResync & (1u << side));
        bool ok = full ? original(owner, pixels, 0, 0, 320, 240, side, origin, stride)
                       : original(owner, pixels, x, y, w, h, side, origin, stride);
        if (ok && full)
            nativeResync &= ~(1u << side);
        return ok;
    });
}

// ZH: 只在唯一入口验证后安装钩子；失败由上层警告并继续使用原生屏幕。
// EN: Install hooks only after unique-entry validation; report failures upstream and retain native screen output.
inline bool start(const std::wstring &path)
{
    auto failure = [&](const char *event, DWORD code) {
        runtimediag::pathEvent(path, "DLL.screen", "ERROR", event, code);
        return false;
    };
    if (target)
        return failure("hook_already_initialized", ERROR_ALREADY_EXISTS);
    target = reinterpret_cast<void *>(vdjcompat::addresses.rect);
    if (!target || !vdjcompat::addresses.displayVtable)
        return failure("entry_unavailable", ERROR_NOT_FOUND);
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
        return failure("minhook_initialize_failed", static_cast<DWORD>(status));
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&rect), &pinned))
        return failure("module_pin_failed", GetLastError());
    if (!pump.start(path))
        return failure("pump_start_failed", GetLastError());
    status = MH_CreateHook(target, reinterpret_cast<void *>(&rect), reinterpret_cast<void **>(&original));
    if (status != MH_OK)
    {
        pump.stop();
        return failure("hook_create_failed", static_cast<DWORD>(status));
    }
    enabled.store(true);
    status = MH_EnableHook(target);
    if (status != MH_OK)
    {
        enabled.store(false);
        pump.stop();
        return failure("hook_enable_failed", static_cast<DWORD>(status));
    }
    return true;
}

inline void bind(ULONG_PTR owner, HANDLE pipe)
{
    std::lock_guard<std::mutex> guard(modeLock);
    if (enabled.load() && takeover)
        pump.bind(owner, pipe);
}

// ZH: 切换接管模式先撤销旧租约，回到原生时要求每侧完整重同步一次。
// EN: Revoke old leases before changing takeover mode; returning to native requires one full resync per side.
inline bool setTakeover(bool on, ULONG_PTR owner = 0, HANDLE pipe = nullptr)
{
    std::lock_guard<std::mutex> guard(modeLock);
    if (takeover == on)
        return false;
    pump.unbind();
    takeover = on;
    nativeResync = 3;
    if (on && enabled.load() && owner && pipe)
        pump.bind(owner, pipe);
    return true;
}

inline void unbind()
{
    if (enabled.load())
        pump.unbind();
}

inline void idle()
{
    if (enabled.load())
        pump.unbind();
}

inline void stop()
{
    pump.stop();
    enabled.store(false);
    if (original)
        MH_DisableHook(target);
}
}
