// ZH: 主Cue、Hotcue和Saved Loop的SDK采集、触觉时间线及NI触觉数据包。
// EN: SDK collection of main cue, hotcues and saved loops, plus haptic timelines and NI haptic packets.
#pragma once

// ZH: 触觉时间线共享内存协议及 NI 数据包。校验曲目身份和时间单位，失败时清空当前侧，不复用旧曲目的 Cue。
// EN: Haptic timeline shared-memory protocol and NI packets. Validate track identity and units; clear the side on failure instead of reusing old cues.

#include <windows.h>
#include <cmath>
#include <cstring>
#include <vector>

namespace hc90
{
constexpr DWORD Magic = 0x53344843, Version = 1, List = 0x036a7365, Position = 0x036a7370, TouchGate = 0x036a7354;
constexpr unsigned MaxRecords = 4096, MaxEntries = MaxRecords * 2;

struct Entry
{
    DWORD id = 0, type = 1;
    float seconds = 0;
};

static_assert(sizeof(Entry) == 12);

struct Side
{
    DWORD deck = 0, enabled = 0, count = 0, padding = 0;
    ULONGLONG track = 0;
    double position = 0;
    Entry entries[MaxEntries]{};
};

struct Frame
{
    DWORD magic = Magic, version = Version, pid = 0, sequence = 0;
    ULONGLONG created = 0, tick = 0, generation = 0;
    Side sides[2]{};
};

inline bool valid(const Side &s)
{
    if (s.deck > 4 || s.enabled > 1 || s.count > MaxEntries || !std::isfinite(s.position) || s.position < 0 ||
        s.position > 1e7)
        return false;
    for (unsigned j = 0; j < s.count; ++j)
        if ((s.entries[j].type != 1 && s.entries[j].type != 5) || !std::isfinite(s.entries[j].seconds) ||
            s.entries[j].seconds < 0 || s.entries[j].seconds > 1e7)
            return false;
    return !s.enabled || (s.deck && s.track);
}

inline bool coherent(const Frame &a, const Frame &b, DWORD pid, ULONGLONG created, ULONGLONG generation, ULONGLONG now)
{
    return !(a.sequence & 1) && !memcmp(&a, &b, sizeof(a)) && a.magic == Magic && a.version == Version &&
           a.pid == pid && a.created == created && a.generation == generation && a.tick <= now && now - a.tick <= 100 &&
           valid(a.sides[0]) && valid(a.sides[1]);
}

// ZH: 快照读取完成后再取时间：本循环执行电机或管道操作时，
// EN: Read time after the snapshot: the publisher may advance while this loop
// ZH: 发布者可能已前进，循环起始时间会错误拒绝更新的有效帧。
// EN: handles motor/pipe work. A loop-start time can reject a valid newer frame.
inline bool publishing(const Frame &a, const Frame &b)
{
    return (a.sequence & 1) || (b.sequence & 1) || memcmp(&a, &b, sizeof(a));
}

template <class Read, class Clock>
bool capture(Frame &a, Frame &b, DWORD pid, ULONGLONG created, ULONGLONG generation, Read read, Clock clock)
{
    return read(a, b) && coherent(a, b, pid, created, generation, clock());
}

struct Packet
{
    std::vector<BYTE> bytes;
};

inline Packet listPacket(unsigned side, const Side &s)
{
    Packet p;
    if (side > 1 || !valid(s))
        return p;
    DWORD count = s.enabled ? s.count : 0;
    p.bytes.resize(16 + 12 * count);
    DWORD head[] = {List, side, count, 0};
    memcpy(p.bytes.data(), head, 16);
    if (count)
        memcpy(p.bytes.data() + 16, s.entries, count * 12);
    return p;
}

inline Packet gatePacket(unsigned side, bool enabled)
{
    Packet p;
    if (side > 1)
        return p;
    DWORD words[] = {TouchGate, side, enabled ? 0x74727565u : 0u};
    p.bytes.resize(12);
    memcpy(p.bytes.data(), words, 12);
    return p;
}

inline Packet positionPacket(unsigned side, float seconds)
{
    Packet p;
    if (side > 1 || !std::isfinite(seconds) || seconds < 0 || seconds > 1e7)
        return p;
    DWORD words[] = {Position, side, 0};
    memcpy(words + 2, &seconds, 4);
    p.bytes.resize(12);
    memcpy(p.bytes.data(), words, 12);
    return p;
}

struct Controller
{
    Side previous[2]{};
    bool known[2]{};
    float lastPosition[2]{};
    bool positionKnown[2]{}, gateKnown[2]{}, gateEnabled[2]{};

    template <class Send> bool update(unsigned i, const Side &s, Send send)
    {
        if (i > 1 || !valid(s))
            return false;
        if (!s.enabled && (!gateKnown[i] || gateEnabled[i]))
        {
            if (!send(gatePacket(i, false)))
                return false;
            gateKnown[i] = true;
            gateEnabled[i] = false;
        }
        auto &old = previous[i];
        bool identity = !known[i] || old.deck != s.deck || old.track != s.track;
        bool changed = identity || old.enabled != s.enabled || old.count != s.count ||
                       memcmp(old.entries, s.entries, s.count * sizeof(Entry));
        if (identity && known[i])
        {
            Side empty;
            if (!send(listPacket(i, empty)))
                return false;
            positionKnown[i] = false;
        }
        if (changed)
        {
            if (!send(listPacket(i, s)))
                return false;
            old = s;
            known[i] = true;
        }
        float pos = s.enabled ? static_cast<float>(s.position) : 0;
        if (!positionKnown[i] || pos != lastPosition[i] || changed)
        {
            if (!send(positionPacket(i, pos)))
                return false;
            lastPosition[i] = pos;
            positionKnown[i] = true;
        }
        if (s.enabled && (!gateKnown[i] || !gateEnabled[i]))
        {
            if (!send(gatePacket(i, true)))
                return false;
            gateKnown[i] = gateEnabled[i] = true;
        }
        return true;
    }

    template <class Send> bool clear(Send send)
    {
        bool ok = true;
        for (unsigned i = 0; i < 2; ++i)
        {
            Side empty;
            if (!send(gatePacket(i, false)) || !send(listPacket(i, empty)) || !send(positionPacket(i, 0)))
                ok = false;
            known[i] = positionKnown[i] = gateKnown[i] = gateEnabled[i] = false;
        }
        return ok;
    }
};

struct Writer
{
    HANDLE pipe = nullptr;
    const BYTE *data = nullptr;
    DWORD size = 0, error = 0;
};

inline DWORD WINAPI writeThread(void *raw)
{
    auto p = static_cast<Writer *>(raw);
    DWORD n = 0;
    if (!WriteFile(p->pipe, p->data, p->size, &n, nullptr))
        p->error = GetLastError();
    else if (n != p->size)
        p->error = ERROR_WRITE_FAULT;
    return 0;
}

inline bool send(HANDLE pipe, const Packet &packet)
{
    if (packet.bytes.empty() || packet.bytes.size() > 16 + 12 * MaxEntries)
        return false;
    Writer w{pipe, packet.bytes.data(), static_cast<DWORD>(packet.bytes.size()), 0};
    HANDLE t = CreateThread(nullptr, 0, writeThread, &w, 0, nullptr);
    if (!t)
        return false;
    DWORD wait = WaitForSingleObject(t, 200);
    if (wait != WAIT_OBJECT_0)
    {
        CancelSynchronousIo(t);
        WaitForSingleObject(t, INFINITE);
    }
    DWORD waitError = wait == WAIT_FAILED ? GetLastError() : 0;
    CloseHandle(t);
    bool ok = wait == WAIT_OBJECT_0 && !w.error;
    if (!ok)
        SetLastError(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : waitError ? waitError : w.error);
    return ok;
}

class Channel
{
    HANDLE mapping = nullptr;
    const Frame *view = nullptr;

  public:
    ~Channel()
    {
        if (view)
            UnmapViewOfFile(view);
        if (mapping)
            CloseHandle(mapping);
    }

    bool open(DWORD pid, const wchar_t *token)
    {
        wchar_t name[128];
        swprintf_s(name, L"Local\\S4VDJHapticV1_%lu_%s", pid, token);
        mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
        if (!mapping)
            return false;
        view = static_cast<const Frame *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(Frame)));
        return view != nullptr;
    }

    bool snapshot(Frame &a, Frame &b)
    {
        if (!view)
            return false;
        memcpy(&a, view, sizeof(a));
        MemoryBarrier();
        memcpy(&b, view, sizeof(b));
        return true;
    }
};
}

// ZH: 仅使用 VDJ SDK 采集路径、主 Cue、Hotcue 和 Saved Loop。采集前后核对身份，避免换曲混入旧时间线。
// EN: Collect paths, main cues, hotcues and saved loops via the VDJ SDK only; check identity before and after capture to avoid mixed-track timelines.

#include <memory>
#include <string>

namespace vh90
{
// ZH: Cue 位置与曲目身份只通过官方 SDK 读取。
// EN: Cue positions and track identity use official SDK only.
template <class Query, class Text> bool snapshot(unsigned deck, hc90::Side &s, Query query, Text text)
{
    s = {};
    if (deck < 1 || deck > 4)
        return false;
    std::string prefix = "deck " + std::to_string(deck) + " ";
    auto number = [&](const std::string &q, double &v) {
        return query((prefix + q).c_str(), v) && std::isfinite(v);
    };
    std::string before, after;
    if (!text((prefix + "get_filepath").c_str(), before) || before.empty())
        return false;
    double loaded = 0, position = 0, main = 0;
    if (!number("loaded", loaded) || loaded != 1 || !number("get_time 'elapsed' 'absolute'", position) ||
        !number("cue_pos 0 mseconly", main) || main < 0 || position < 0)
        return false;
    s.deck = deck;
    s.enabled = 1;
    s.position = position / 1000;
    ULONGLONG hash = 14695981039346656037ull;
    for (unsigned char c : before)
    {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    s.track = hash ? hash : 1;
    s.entries[s.count++] = {0, 1, static_cast<float>(main / 1000)};
    for (unsigned n = 1; n <= 8; ++n)
    {
        double exists = 0, pos = 0;
        std::string id = std::to_string(n);
        if (!number("has_cue " + id, exists))
            return false;
        if (exists != 1)
            continue;
        if (!number("cue_pos " + id + " mseconly", pos) || pos < 0)
            return false;
        s.entries[s.count++] = {n, 1, static_cast<float>(pos / 1000)};
    }
    // ZH: Saved Loop 槽位是独立来源，不合并位置相同的记录。
    // EN: Saved Loop slots are a separate source; do not merge equal positions.
    for (unsigned n = 1; n <= 8; ++n)
    {
        double exists = 0, start = 0, length = 0;
        std::string id = std::to_string(n);
        if (!number("saved_loop " + id, exists))
            return false;
        if (exists != 1)
            continue;
        if (!number("get_saved_loop " + id + " 'pos'", start) || !number("get_saved_loop " + id + " 'len'", length) ||
            start < 0 || length <= 0)
            return false;
        double end = start + length;
        if (!std::isfinite(end) || end > 1e7)
            return false;
        // ZH: 独立命名空间保留 Hotcue 与 Loop 各自的记录身份。
        // EN: Independent namespace preserves the Hotcue and Loop record identities.
        s.entries[s.count++] = {0x10000u + n, 5, static_cast<float>(start)};
        s.entries[s.count++] = {0x10000u + n, 5, static_cast<float>(end)};
    }
    return text((prefix + "get_filepath").c_str(), after) && before == after && hc90::valid(s);
}

class Publisher
{
    std::string failure[2];
    bool contentChanged = false;
    HANDLE mapping = nullptr;
    hc90::Frame *view = nullptr;
    std::unique_ptr<hc90::Frame> next{new hc90::Frame};

  public:
    const std::string &lastFailure(unsigned side) const
    {
        return failure[side];
    }

    bool changed() const
    {
        return contentChanged;
    }

    ~Publisher()
    {
        if (view)
            UnmapViewOfFile(view);
        if (mapping)
            CloseHandle(mapping);
    }

    bool create(DWORD pid, ULONGLONG created, const wchar_t *token)
    {
        wchar_t name[128];
        swprintf_s(name, L"Local\\S4VDJHapticV1_%lu_%s", pid, token);
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(hc90::Frame), name);
        if (!mapping || GetLastError() == ERROR_ALREADY_EXISTS)
            return false;
        view = static_cast<hc90::Frame *>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(hc90::Frame)));
        if (!view)
            return false;
        *view = {};
        view->pid = pid;
        view->created = created;
        return true;
    }

    // ZH: 采集失败发布空侧；下一次成功立即恢复，不锁死错误、不重放过期触觉。
    // EN: Publish an empty side on capture failure; recover on the next success without latching errors or replaying stale haptics.
    template <class Phase, class Query, class Text>
    unsigned publish(const Phase &phase, bool enabled, Query query, Text text)
    {
        contentChanged = false;
        if (!view)
            return 2;
        next->pid = GetCurrentProcessId();
        next->created = view->created;
        next->generation = phase.generation;
        next->tick = GetTickCount64();
        unsigned errors = 0;
        for (unsigned i = 0; i < 2; ++i)
        {
            auto &s = next->sides[i];
            s = {};
            failure[i].clear();
            if (!enabled || !(phase.valid & (1u << i)) || !(phase.facts[i] & 1) || (phase.facts[i] & 2))
                continue;
            double before = 0, after = 0;
            const char *alias = i ? "deck right get_deck" : "deck left get_deck";
            auto checkedQuery = [&](const char *q, double &v) {
                bool ok = query(q, v);
                if (!ok || !std::isfinite(v))
                {
                    failure[i] = std::string("query_failed: ") + q;
                    return false;
                }
                return true;
            };
            auto checkedText = [&](const char *q, std::string &v) {
                bool ok = text(q, v);
                if (!ok || v.empty())
                    failure[i] = std::string("text_failed: ") + q;
                return ok;
            };
            bool ok = checkedQuery(alias, before) && before == phase.deck[i] &&
                      snapshot(phase.deck[i], s, checkedQuery, checkedText) && checkedQuery(alias, after) &&
                      after == before;
            if (!ok && failure[i].empty())
                failure[i] = "snapshot_value_or_identity_rejected";
            if (!ok)
            {
                s = {};
                ++errors;
            }
        }
        // ZH: 只比较时间线内容及代次，心跳仍持续更新；禁用/暂停且内容不变时不产生重复唤醒。
        // EN: Compare timeline payload and generation only; heartbeats still advance without redundant wakes for unchanged disabled/paused state.
        contentChanged = view->generation != next->generation || memcmp(view->sides, next->sides, sizeof(view->sides));
        InterlockedIncrement(reinterpret_cast<volatile LONG *>(&view->sequence));
        constexpr size_t offset = offsetof(hc90::Frame, sequence);
        memcpy(view, next.get(), offset);
        memcpy(reinterpret_cast<BYTE *>(view) + offset + 4, reinterpret_cast<const BYTE *>(next.get()) + offset + 4,
               sizeof(*view) - offset - 4);
        MemoryBarrier();
        InterlockedIncrement(reinterpret_cast<volatile LONG *>(&view->sequence));
        return errors;
    }
};
}
