// ZH: NI服务兼容适配：PE与特征定位、字段解析校验、动态灯环缓存访问及失败告警。
// EN: NI service compatibility: PE/signature discovery, validated field decoding, dynamic ring-cache access and failure warnings.
#pragma once
#include "runtime_utils.h"

// ZH: NI 内部入口与字段解析：先验证已知位置，再回退特征搜索；重复字段必须一致，失败只关闭相关自定义功能。
// EN: NI entry and field resolution: validate known locations before scanning; duplicate fields must agree, and failure disables only affected custom features.

#include <windows.h>
#include <tlhelp32.h>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include "signatures/ni_service_signatures.h"
#include "jog_ring.h"
#pragma comment(lib, "user32.lib")

namespace nicompat
{
struct Section
{
    DWORD rva = 0, flags = 0;
    size_t extent = 0;
    std::vector<unsigned char> data;
};

struct Image
{
    uintptr_t base = 0;
    DWORD size = 0;
    HANDLE process = nullptr;
    std::vector<Section> sections;

    bool read(uintptr_t at, void *out, size_t n) const
    {
        for (const auto &s : sections)
        {
            size_t len = s.extent ? s.extent : s.data.size();
            if (at >= base + s.rva && n <= len && at - base - s.rva <= len - n)
            {
                if (!s.data.empty())
                {
                    memcpy(out, s.data.data() + at - base - s.rva, n);
                    return true;
                }
                SIZE_T got = 0;
                return process && ReadProcessMemory(process, reinterpret_cast<void *>(at), out, n, &got) && got == n;
            }
        }
        return false;
    }

    template <class T> bool get(uintptr_t at, T &v) const
    {
        return read(at, &v, sizeof(v));
    }

    bool region(uintptr_t at, size_t n, bool code) const
    {
        for (const auto &s : sections)
        {
            size_t len = s.extent ? s.extent : s.data.size();
            if (bool(s.flags & IMAGE_SCN_MEM_EXECUTE) == code && !(s.flags & IMAGE_SCN_MEM_WRITE) &&
                at >= base + s.rva && n <= len && at - base - s.rva <= len - n)
                return true;
        }
        return false;
    }

    bool load(HANDLE target, DWORD pid, bool snapshot = true)
    {
        process = target;
        sections.clear();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
        if (snap == INVALID_HANDLE_VALUE)
            return false;
        MODULEENTRY32W mod{};
        mod.dwSize = sizeof(mod);
        bool ok = Module32FirstW(snap, &mod) != FALSE;
        CloseHandle(snap);
        if (!ok)
            return false;
        base = reinterpret_cast<uintptr_t>(mod.modBaseAddr);
        auto remote = [&](uintptr_t at, void *out, size_t n) {
            SIZE_T got = 0;
            return ReadProcessMemory(process, reinterpret_cast<void *>(at), out, n, &got) && got == n;
        };
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        if (!remote(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
            dos.e_lfanew > 0x100000 || !remote(base + dos.e_lfanew, &nt, sizeof(nt)) ||
            nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            return false;
        size = nt.OptionalHeader.SizeOfImage;
        if (size < 4096 || size > 256 * 1024 * 1024 || !nt.FileHeader.NumberOfSections ||
            nt.FileHeader.NumberOfSections > 96)
            return false;
        auto table =
            base + dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
        size_t total = 0;
        for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; ++i)
        {
            IMAGE_SECTION_HEADER h{};
            if (!remote(table + i * sizeof(h), &h, sizeof(h)))
                return false;
            DWORD n = h.Misc.VirtualSize;
            if (!n)
                continue;
            if (h.VirtualAddress >= size || n > size - h.VirtualAddress)
                return false;
            if (!(h.Characteristics & IMAGE_SCN_MEM_READ))
                continue;
            total += n;
            if (total > 128 * 1024 * 1024)
                return false;
            Section s;
            s.rva = h.VirtualAddress;
            s.flags = h.Characteristics;
            s.extent = n;
            if (snapshot)
            {
                s.data.resize(n);
                if (!remote(base + s.rva, s.data.data(), n))
                    return false;
            }
            sections.push_back(std::move(s));
        }
        return !sections.empty();
    }

    bool matches(uintptr_t at, const Pattern &p) const
    {
        if (!region(at, p.size, true))
            return false;
        std::vector<unsigned char> bytes(p.size);
        if (!read(at, bytes.data(), bytes.size()))
            return false;
        for (size_t i = 0; i < p.size; ++i)
            if ((bytes[i] & p.mask[i]) != (p.bytes[i] & p.mask[i]))
                return false;
        return true;
    }

    uintptr_t vtableAt(uintptr_t vt, const char *name) const
    {
        uintptr_t col = 0, fn = 0;
        DWORD f[6]{};
        if (!region(vt - 8, 40, false) || !get(vt - 8, col) || !region(col, 24, false) || !read(col, f, 24) ||
            f[0] != 1 || f[1] || f[2] || f[5] != col - base)
            return 0;
        size_t n = strlen(name) + 1;
        char actual[128]{};
        if (n > sizeof(actual) || !read(base + f[3] + 16, actual, n) || memcmp(actual, name, n))
            return 0;
        for (unsigned slot = 0; slot < 4; ++slot)
            if (!get(vt + slot * 8, fn) || !region(fn, 1, true))
                return 0;
        return vt;
    }

    template <class Accept> uintptr_t pattern(const Pattern &p, Accept accept) const
    {
        size_t anchor = 0, best = 0, run = 0;
        for (size_t i = 0; i < p.size; ++i)
        {
            run = p.mask[i] == 255 ? run + 1 : 0;
            if (run > best)
            {
                best = run;
                anchor = i + 1 - run;
            }
        }
        if (best < 3)
            return 0;
        uintptr_t found = 0;
        for (const auto &s : sections)
        {
            if (!(s.flags & IMAGE_SCN_MEM_EXECUTE) || s.flags & IMAGE_SCN_MEM_WRITE || s.data.size() < p.size)
                continue;
            auto begin = s.data.begin(), end = s.data.end();
            while (begin != end)
            {
                auto pos = std::search(begin, end, p.bytes + anchor, p.bytes + anchor + best);
                if (pos == end)
                    break;
                size_t at = static_cast<size_t>(pos - s.data.begin());
                begin = pos + 1;
                if (at < anchor)
                    continue;
                at -= anchor;
                if (at > s.data.size() - p.size)
                    continue;
                bool same = true;
                for (size_t i = 0; i < p.size; ++i)
                    if ((s.data[at + i] & p.mask[i]) != (p.bytes[i] & p.mask[i]))
                    {
                        same = false;
                        break;
                    }
                if (same && accept(base + s.rva + at))
                {
                    if (found)
                        return 0;
                    found = base + s.rva + at;
                }
            }
        }
        return found;
    }

    uintptr_t pattern(const Pattern &p) const
    {
        return pattern(p, [](uintptr_t) {
            return true;
        });
    }

    uintptr_t vtable(const char *name) const
    {
        uintptr_t result = 0;
        size_t len = strlen(name) + 1;
        for (const auto &s : sections)
        {
            if (s.flags & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_WRITE))
                continue;
            for (size_t i = 0; i + 8 <= s.data.size(); i += 8)
            {
                uintptr_t col = 0;
                memcpy(&col, s.data.data() + i, 8);
                DWORD fields[6]{};
                if (!region(col, sizeof(fields), false) || !read(col, fields, sizeof(fields)) || fields[0] != 1 ||
                    fields[1] != 0 || fields[2] != 0 || fields[5] != col - base)
                    continue;
                char actual[128]{};
                if (len > sizeof(actual) || !read(base + fields[3] + 16, actual, len) || memcmp(actual, name, len))
                    continue;
                uintptr_t vt = base + s.rva + i + 8;
                bool valid = true;
                for (unsigned slot = 0; slot < 4; ++slot)
                {
                    uintptr_t fn = 0;
                    if (!get(vt + 8 * slot, fn) || !region(fn, 1, true))
                    {
                        valid = false;
                        break;
                    }
                }
                if (valid)
                {
                    if (result)
                        return 0;
                    result = vt;
                }
            }
        }
        return result;
    }
};

struct Layout
{
    size_t serverDevice = 0, focus = 0, deviceJog = 0, sideBase[2]{}, motion[2]{}, mode[2]{}, enabled[2]{}, scale = 0,
           cache[2]{}, color[2]{}, angle[2]{};
};

inline bool sameLayout(const Layout &a, const Layout &b)
{
    return !memcmp(&a, &b, sizeof(a));
}

// ZH: 仅作为测试样本或已知候选，绝不作为未验证的兜底。
// EN: Only a test fixture / known candidate, never an unvalidated fallback.
inline constexpr Layout KnownLayout{0x2b8,           0x178,           0x248,  {0x80, 0x1040},   {0xc8, 0x1088},
                                    {0xcc, 0x108c},  {0x2f4, 0x12b4}, 0x2004, {0x2044, 0x208c}, {0x2048, 0x2090},
                                    {0x204a, 0x2092}};

struct Result
{
    uintptr_t serverVtable = 0;
    const char *reason = "not_checked";
    uintptr_t deviceVtable = 0;
    Layout layout{};
    bool usedScan = true;

    explicit operator bool() const
    {
        return serverVtable != 0;
    }
};

inline bool extractLayout(const Image &image, const uintptr_t *locations, Layout &out)
{
    size_t values[FieldCount]{};
    bool seen[FieldCount]{};
    for (const auto &site : FieldSites)
    {
        unsigned char bytes[4]{};
        if (site.width != 1 && site.width != 2 && site.width != 4)
            return false;
        if (!image.read(locations[site.pattern] + site.offset, bytes, site.width))
            return false;
        int32_t value = 0;
        if (site.width == 1)
            value = static_cast<signed char>(bytes[0]);
        else if (site.width == 2)
        {
            uint16_t v = 0;
            memcpy(&v, bytes, 2);
            value = v;
        }
        else
            memcpy(&value, bytes, 4);
        value *= site.sign;
        if (value <= 0 || value > 0x10000)
            return false;
        size_t v = static_cast<size_t>(value);
        if (seen[site.field] && values[site.field] != v)
            return false;
        seen[site.field] = true;
        values[site.field] = v;
    }
    for (bool present : seen)
        if (!present)
            return false;
    if (values[FocusOwner] != values[Focus] + 8 || values[FocusRole] != values[Focus] + 16)
        return false;
    Layout l;
    l.serverDevice = values[ServerDevice];
    l.focus = values[Focus];
    l.deviceJog = values[DeviceJog];
    l.sideBase[0] = values[SideLeft];
    l.sideBase[1] = values[SideRight];
    l.motion[0] = values[MotionLeft];
    l.motion[1] = values[MotionRight];
    l.mode[0] = values[ModeLeft];
    l.mode[1] = values[ModeRight];
    l.scale = values[Scale];
    l.cache[0] = values[CacheLeft];
    l.cache[1] = values[CacheRight];
    l.color[0] = values[ColorLeft];
    l.color[1] = values[ColorRight];
    l.angle[0] = values[AngleLeft];
    l.angle[1] = values[AngleRight];
    size_t stride = l.sideBase[1] - l.sideBase[0];
    if (l.sideBase[1] <= l.sideBase[0] || stride < 128 || stride > 0x8000)
        return false;
    for (unsigned i = 0; i < 2; ++i)
    {
        if (l.motion[i] != l.sideBase[i] + values[ChildMotion] || l.mode[i] != l.sideBase[i] + values[ChildMode])
            return false;
        l.enabled[i] = l.sideBase[i] + values[ChildEnabled];
        if (l.serverDevice % 8 || l.focus % 8 || l.deviceJog % 8 || l.cache[i] % 4 || l.angle[i] % 2 || l.mode[i] % 4 ||
            l.scale % 4)
            return false;
        if (l.color[i] < l.cache[i] + 4 || l.color[i] - l.cache[i] > 32 || l.angle[i] <= l.color[i] ||
            l.angle[i] - l.cache[i] > 48)
            return false;
        if (l.enabled[i] >= l.sideBase[i] + stride || l.motion[i] >= l.sideBase[i] + stride ||
            l.mode[i] + 4 > l.sideBase[i] + stride)
            return false;
    }
    if (l.motion[1] - l.motion[0] != stride || l.mode[1] - l.mode[0] != stride ||
        l.cache[1] - l.cache[0] != l.color[1] - l.color[0] || l.cache[1] - l.cache[0] != l.angle[1] - l.angle[0])
        return false;
    out = l;
    return true;
}

inline Result resolve(const Image &image, bool known = false)
{
    auto vt = known ? image.vtableAt(image.base + 0x5857f8, ".?AVTraktorKontrolS4MK3@SERVER@NHL2@NI@@")
                    : image.vtable(".?AVTraktorKontrolS4MK3@SERVER@NHL2@NI@@");
    if (!vt)
        return {0, "S4_server_RTTI"};
    uintptr_t locations[sizeof(Patterns) / sizeof(Patterns[0])]{};
    for (size_t i = 0; i < sizeof(Patterns) / sizeof(Patterns[0]); ++i)
    {
        if (known)
        {
            auto at = image.base + KnownFunctions[i];
            if (image.matches(at, Patterns[i]))
                locations[i] = at;
        }
        else if (i == 0)
        {
            uintptr_t fn = 0;
            if (image.get(vt + 8, fn) && image.matches(fn, Patterns[i]))
                locations[i] = fn;
        }
        else if (i == 8)
        {
            auto field = [&](Field f) -> size_t {
                for (const auto &site : FieldSites)
                    if (site.field == f && locations[site.pattern])
                    {
                        unsigned char bytes[4]{};
                        if (!image.read(locations[site.pattern] + site.offset, bytes, site.width))
                            return 0;
                        int32_t v = 0;
                        if (site.width == 1)
                            v = static_cast<signed char>(bytes[0]);
                        else
                            memcpy(&v, bytes, 4);
                        v *= site.sign;
                        return v > 0 ? static_cast<size_t>(v) : 0;
                    }
                return 0;
            };
            size_t expected[2] = {field(SideLeft) + field(ChildMode), field(SideRight) + field(ChildMode)};
            locations[i] = image.pattern(Patterns[i], [&](uintptr_t at) {
                for (const auto &site : FieldSites)
                    if (site.pattern == i)
                    {
                        DWORD v = 0;
                        if (site.width != 4 || !image.get(at + site.offset, v))
                            return false;
                        auto index = site.field == ModeLeft ? 0 : 1;
                        if (v != expected[index])
                            return false;
                    }
                return true;
            });
        }
        else
            locations[i] = image.pattern(Patterns[i]);
        if (!locations[i])
            return {0, Patterns[i].name};
    }
    // ZH: 通过匹配指针确定方法槽，不假定槽偏移。
    // EN: Determine the method slots from matching pointers rather than assume slot offsets.
    unsigned getterCount = 0, focusCount = 0;
    for (unsigned i = 0; i < 128; ++i)
    {
        uintptr_t fn = 0;
        if (!image.get(vt + i * 8, fn) || !image.region(fn, 1, true))
            break;
        if (fn == locations[0])
            ++getterCount;
        if (fn == locations[1])
            ++focusCount;
    }
    if (!getterCount || !focusCount)
        return {0, "S4_vtable_links"};
    auto target = [&](uintptr_t at) {
        unsigned char op = 0;
        int32_t delta = 0;
        return image.get(at, op) && op == 0xe8 && image.get(at + 1, delta) ? at + 5 + delta : uintptr_t(0);
    };
    if (target(locations[2] + 0x927) != locations[3] || target(locations[3] + 0x42) != locations[4] ||
        target(locations[3] + 0x52) != locations[4])
        return {0, "NI_constructor_links"};
    auto device = known ? image.vtableAt(image.base + 0xb2b428, ".?AVDevice@KontrolS4MK3@Traktor@NHL2@NI@@")
                        : image.vtable(".?AVDevice@KontrolS4MK3@Traktor@NHL2@NI@@");
    if (!device)
        return {0, "S4_device_RTTI"};
    Layout layout;
    if (!extractLayout(image, locations, layout))
        return {0, "NI_field_relations"};
    return {vt, known ? "known_locations_fields_parsed" : "feature_scan_fields_parsed", device, layout, !known};
}

// ZH: 先走已知候选的完整校验，失败才扫描；缓存位置不能绕过字段一致性检查。
// EN: Fully validate known candidates before scanning; cached locations cannot bypass field consistency checks.
inline Result locate(HANDLE process, DWORD pid)
{
    try
    {
        Image image;
        if (!image.load(process, pid, false))
            return {0, "NI_PE_read"};
        auto fast = resolve(image, true);
        if (fast)
            return fast;
        if (!image.load(process, pid, true))
            return {0, "NI_PE_read"};
        return resolve(image);
    }
    catch (...)
    {
        return {0, "NI_locator_exception"};
    }
}

inline bool cacheRead(HANDLE process, uintptr_t jog, const Layout &l, unsigned side, ring18::Cache &out)
{
    if (side > 1 || !l.cache[side] || l.color[side] < l.cache[side] + 4 || l.angle[side] <= l.color[side])
        return false;
    size_t n = l.angle[side] + 2 - l.cache[side];
    if (n > 64)
        return false;
    BYTE bytes[64]{};
    SIZE_T got = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<void *>(jog + l.cache[side]), bytes, n, &got) || got != n)
        return false;
    memcpy(&out.mode, bytes, 4);
    out.color = bytes[l.color[side] - l.cache[side]];
    out.reserved = 0;
    memcpy(&out.angle, bytes + l.angle[side] - l.cache[side], 2);
    return true;
}

inline bool cacheWrite(HANDLE process, uintptr_t jog, const Layout &l, unsigned side, const ring18::Cache &value)
{
    if (side > 1 || !l.cache[side] || l.color[side] < l.cache[side] + 4 || l.angle[side] <= l.color[side])
        return false;
    size_t n = l.angle[side] + 2 - l.cache[side];
    if (n > 64)
        return false;
    BYTE bytes[64]{};
    SIZE_T got = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<void *>(jog + l.cache[side]), bytes, n, &got) || got != n)
        return false;
    memcpy(bytes, &value.mode, 4);
    bytes[l.color[side] - l.cache[side]] = value.color;
    memcpy(bytes + l.angle[side] - l.cache[side], &value.angle, 2);
    return WriteProcessMemory(process, reinterpret_cast<void *>(jog + l.cache[side]), bytes, n, &got) && got == n;
}

inline void report(FILE *log, const Result &r)
{
    if (!log)
        return;
    SYSTEMTIME t{};
    GetLocalTime(&t);
    liveLogPrintf(
        log,
        "%04u-%02u-%02uT%02u:%02u:%02u %s compatibility feature=ni_output mode=%s reason=%s server_vtable=%p; native VDJ, cold-start input and screen output retained\n",
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, r ? "INFO" : "WARNING",
        r.usedScan ? "feature_scan" : "known_location_validated", r.reason, reinterpret_cast<void *>(r.serverVtable));
    liveLogPrintf(
        log,
        "NI_LAYOUT server_device=%zx focus=%zx device_jog=%zx side_left=%zx side_right=%zx motion=%zx,%zx mode=%zx,%zx enabled=%zx,%zx scale=%zx cache=%zx,%zx color=%zx,%zx angle=%zx,%zx\n",
        r.layout.serverDevice, r.layout.focus, r.layout.deviceJog, r.layout.sideBase[0], r.layout.sideBase[1],
        r.layout.motion[0], r.layout.motion[1], r.layout.mode[0], r.layout.mode[1], r.layout.enabled[0],
        r.layout.enabled[1], r.layout.scale, r.layout.cache[0], r.layout.cache[1], r.layout.color[0], r.layout.color[1],
        r.layout.angle[0], r.layout.angle[1]);
    fflush(log);
}

inline void warn(FILE *log, const Result &r)
{
    report(log, r);
    static std::atomic<bool> shown{false};
    if (shown.exchange(true))
        return;
    MessageBoxW(
        nullptr,
        L"NIHardwareService compatibility validation failed. Custom TT motor, jog ring and haptic output are disabled. Native VirtualDJ controls, cold-start input synchronization and screen output remain available. See the NI Service Helper log.",
        L"TK S4MK3 - NI Compatibility Warning", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
}
}
