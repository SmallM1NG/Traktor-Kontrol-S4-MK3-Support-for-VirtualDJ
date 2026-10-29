// ZH: VDJ兼容适配：宿主身份、入口与字段定位、逐功能可用性校验和失败告警。
// EN: VDJ compatibility: host identity, entry/field discovery, per-feature validation and failure warnings.
#pragma once
#include "runtime_utils.h"

// ZH: 只读特征匹配工具；搜索范围和通配掩码由上层提供，不写入被扫描代码。
// EN: Read-only pattern matcher: callers supply search bounds and masks; scanned code is never modified.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace vdjscan
{
// ZH: 零表示没有匹配或存在歧义，两种情况均不能作为入口。
// EN: Zero means no match OR ambiguous: neither is admitted as an entry point.
inline size_t unique(const unsigned char *data, size_t size, const unsigned char *pattern, const unsigned char *mask,
                     size_t length, bool *ambiguous = nullptr)
{
    if (ambiguous)
        *ambiguous = false;
    if (!length || size < length)
        return SIZE_MAX;
    size_t found = SIZE_MAX, anchor = 0;
    while (anchor < length && !mask[anchor])
        ++anchor;
    if (anchor == length)
        return SIZE_MAX;
    size_t next = 0, last = size - length;
    while (next <= last)
    {
        const auto hit =
            static_cast<const unsigned char *>(std::memchr(data + next + anchor, pattern[anchor], last - next + 1));
        if (!hit)
            break;
        size_t i = static_cast<size_t>(hit - data) - anchor;
        size_t j = 0;
        for (; j < length; ++j)
            if (mask[j] && data[i + j] != pattern[j])
                break;
        if (j == length)
        {
            if (found != SIZE_MAX)
            {
                if (ambiguous)
                    *ambiguous = true;
                return SIZE_MAX;
            }
            found = i;
        }
        next = i + 1;
    }

    return found;
}
}

// ZH: VDJ 内部对象与入口定位；已知位置也必须验证，再解析字段，验证失败时禁止使用旧偏移兜底。
// EN: Resolve VDJ internal objects/entries; validate known locations before decoding fields and never fall back to unvalidated offsets.

#include <windows.h>
#include <vector>
#include <string>
#include <cstring>
#include <atomic>
#include <new>
#include <chrono>

#include "signatures/vdj_signatures.h"
#include "runtime_utils.h"
#pragma comment(lib, "user32.lib")

namespace vdjcompat
{
inline bool read(uintptr_t at, void *out, size_t size)
{
    SIZE_T n = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(at), out, size, &n) && n == size;
}

template <class T> inline bool read(uintptr_t at, T &out)
{
    return read(at, &out, sizeof(out));
}

struct Section
{
    uintptr_t at = 0;
    DWORD flags = 0;
    size_t extent = 0;
    std::vector<unsigned char> bytes;
};

struct Image
{
    uintptr_t base = 0;
    DWORD size = 0;
    std::vector<Section> sections;

    bool load(HMODULE module = GetModuleHandleW(nullptr), bool snapshot = true)
    {
        sections.clear();
        base = reinterpret_cast<uintptr_t>(module);
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        if (!read(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000 ||
            !read(base + dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            return false;
        size = nt.OptionalHeader.SizeOfImage;
        if (size < 4096 || size > 1024u * 1024u * 1024u || !nt.FileHeader.NumberOfSections ||
            nt.FileHeader.NumberOfSections > 96)
            return false;
        uintptr_t table =
            base + dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
        for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; ++i)
        {
            IMAGE_SECTION_HEADER s{};
            if (!read(table + i * sizeof(s), s))
                return false;
            DWORD len = s.Misc.VirtualSize;
            if (!len)
                continue;
            if (s.VirtualAddress >= size || len > size - s.VirtualAddress)
                return false;
            if (!(s.Characteristics & IMAGE_SCN_MEM_READ))
                continue;
            Section part;
            part.at = base + s.VirtualAddress;
            part.flags = s.Characteristics;
            part.extent = len;
            if (snapshot)
            {
                part.bytes.resize(len);
                if (!read(part.at, part.bytes.data(), len))
                    return false;
            }
            sections.push_back(std::move(part));
        }
        return !sections.empty();
    }

    bool executable(uintptr_t at) const
    {
        for (const auto &s : sections)
            if ((s.flags & IMAGE_SCN_MEM_EXECUTE) && at >= s.at && at - s.at < (s.extent ? s.extent : s.bytes.size()))
                return true;
        return false;
    }

    bool writable(uintptr_t at, size_t n) const
    {
        if (at < base || n > size || at - base > size - n)
            return false;
        MEMORY_BASIC_INFORMATION m{};
        if (!VirtualQuery(reinterpret_cast<void *>(at), &m, sizeof(m)) || m.State != MEM_COMMIT ||
            m.Protect & (PAGE_GUARD | PAGE_NOACCESS))
            return false;
        return (m.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) &&
               at + n <= reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize;
    }

    bool contains(uintptr_t at, size_t n) const
    {
        return at >= base && n <= size && at - base <= size - n;
    }

    bool dataAddress(uintptr_t at, size_t n) const
    {
        if (!contains(at, n))
            return false;
        for (const auto &s : sections)
        {
            size_t extent = s.extent ? s.extent : s.bytes.size();
            if (!(s.flags & IMAGE_SCN_MEM_EXECUTE) && at >= s.at && n <= extent && at - s.at <= extent - n)
                return true;
        }
        return false;
    }

    bool matches(uintptr_t at, const unsigned char *bytes, const unsigned char *mask, size_t n) const
    {
        if (!executable(at) || !contains(at, n) || n > 1024)
            return false;
        unsigned char actual[1024]{};
        if (!read(at, actual, n))
            return false;
        for (size_t i = 0; i < n; ++i)
            if (mask[i] && actual[i] != bytes[i])
                return false;
        return true;
    }

    bool validVtable(uintptr_t at, const char *type) const
    {
        if (at < sizeof(uintptr_t) || !dataAddress(at - sizeof(uintptr_t), 4 * sizeof(uintptr_t)))
            return false;
        uintptr_t col = 0, methods[3]{};
        DWORD f[6]{};
        if (!read(at - sizeof(uintptr_t), col) || !dataAddress(col, sizeof(f)) || !read(col, f) || f[0] != 1 || f[1] ||
            f[2] || f[5] != col - base)
            return false;
        if (!dataAddress(base + f[3], 16) || !dataAddress(base + f[4], 16) || !read(at, methods))
            return false;
        size_t length = strlen(type) + 1;
        if (length > 128 || !dataAddress(base + f[3] + 16, length))
            return false;
        char name[128]{};
        if (!read(base + f[3] + 16, name, length) || memcmp(name, type, length))
            return false;
        for (auto method : methods)
            if (!executable(method))
                return false;
        return true;
    }

    uintptr_t pattern(const unsigned char *bytes, const unsigned char *mask, size_t n) const
    {
        uintptr_t found = 0;
        for (const auto &s : sections)
            if (s.flags & IMAGE_SCN_MEM_EXECUTE)
            {
                bool ambiguous = false;
                auto offset = vdjscan::unique(s.bytes.data(), s.bytes.size(), bytes, mask, n, &ambiguous);
                if (ambiguous)
                    return 0;
                if (offset != SIZE_MAX)
                {
                    if (found)
                        return 0;
                    found = s.at + offset;
                }
            }
        return found;
    }

    uintptr_t vtable(const char *type) const
    {
        uintptr_t descriptor = 0;
        const size_t len = strlen(type) + 1;
        for (const auto &s : sections)
            if (!(s.flags & IMAGE_SCN_MEM_EXECUTE))
            {
                if (s.bytes.size() < 16 + len)
                    continue;
                size_t next = 16, last = s.bytes.size() - len;
                while (next <= last)
                {
                    // ZH: 运行库 memchr 使用向量化搜索，只在候选位置比较名称。
                    // EN: Runtime memchr uses vectorized search; compare names only at candidates.
                    const auto hit = static_cast<const unsigned char *>(
                        memchr(s.bytes.data() + next, static_cast<unsigned char>(type[0]), last - next + 1));
                    if (!hit)
                        break;
                    size_t offset = static_cast<size_t>(hit - s.bytes.data());
                    if (!memcmp(hit, type, len))
                    {
                        if (descriptor)
                            return 0;
                        descriptor = s.at + offset - 16;
                    }
                    next = offset + 1;
                }
            }
        if (!descriptor || descriptor < base || descriptor - base > UINT32_MAX)
            return 0;
        uintptr_t col = 0;
        DWORD td = static_cast<DWORD>(descriptor - base);
        for (const auto &s : sections)
            if (!(s.flags & IMAGE_SCN_MEM_EXECUTE))
                for (size_t i = 0; i + 24 <= s.bytes.size(); i += 4)
                {
                    DWORD fields[6]{};
                    memcpy(fields, s.bytes.data() + i, 24);
                    if (fields[0] == 1 && fields[1] == 0 && fields[2] == 0 && fields[3] == td &&
                        fields[5] == s.at + i - base)
                    {
                        if (col)
                            return 0;
                        col = s.at + i;
                    }
                }
        if (!col)
            return 0;
        uintptr_t found = 0;
        for (const auto &s : sections)
            if (!(s.flags & IMAGE_SCN_MEM_EXECUTE))
                for (size_t i = 0; i + 32 <= s.bytes.size(); i += 8)
                {
                    uintptr_t words[4]{};
                    memcpy(words, s.bytes.data() + i, sizeof(words));
                    if (words[0] == col && executable(words[1]) && executable(words[2]) && executable(words[3]))
                    {
                        if (found)
                            return 0;
                        found = s.at + i + 8;
                    }
                }
        return found;
    }

    bool functionStart(uintptr_t at) const
    {
        if (!at || !executable(at))
            return false;
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        if (!read(base, dos) || !read(base + dos.e_lfanew, nt))
            return false;
        auto d = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (!d.VirtualAddress || d.VirtualAddress >= size || d.Size > size - d.VirtualAddress ||
            d.Size % sizeof(RUNTIME_FUNCTION))
            return false;
        size_t lo = 0, hi = d.Size / sizeof(RUNTIME_FUNCTION);
        while (lo < hi)
        {
            size_t mid = lo + (hi - lo) / 2;
            RUNTIME_FUNCTION f{};
            if (!read(base + d.VirtualAddress + mid * sizeof(f), f))
                return false;
            if (at - base == f.BeginAddress)
                return f.EndAddress > f.BeginAddress && f.EndAddress <= size;
            if (at - base < f.BeginAddress)
                hi = mid;
            else
                lo = mid + 1;
        }
        return false;
    }
};

struct Addresses
{
    uintptr_t displayVtable = 0, inputVtable = 0, displayVector = 0, rect = 0, input = 0;
    size_t displayPipe = 0, inputHandle = 0, inputReportSize = 0, displayType = 0, displayWidth = 0, displayHeight = 0;
};

inline Addresses addresses;

inline bool host()
{
    wchar_t path[32768]{};
    DWORD n = GetModuleFileNameW(nullptr, path, 32768);
    if (!n || n >= 32768)
        return false;
    const auto leaf = wcsrchr(path, L'\\');
    return !_wcsicmp(leaf ? leaf + 1 : path, L"virtualdj.exe");
}

inline uintptr_t rip(uintptr_t instruction, size_t displacement, size_t length)
{
    int32_t d = 0;
    return read(instruction + displacement, d) ? instruction + length + d : 0;
}

inline Addresses profileFields(const Image &image, const LayoutProfile &profile, const uintptr_t *locations,
                               uintptr_t displayVtable, uintptr_t inputVtable, bool newer)
{
    Addresses a;
    a.displayVtable = displayVtable;
    a.inputVtable = inputVtable;
    size_t fields[LayoutFieldCount]{};
    int status[LayoutFieldCount]{};
    for (size_t i = 0; i < profile.siteCount; ++i)
    {
        const auto &site = profile.sites[i];
        if (!locations[site.pattern] || status[site.field] < 0)
            continue;
        unsigned char bytes[4]{};
        if ((site.width != 1 && site.width != 2 && site.width != 4) ||
            !read(locations[site.pattern] + site.offset, bytes, site.width))
        {
            status[site.field] = -1;
            continue;
        }
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
        if (value <= 0 || value > 0x10000 || (status[site.field] && fields[site.field] != static_cast<size_t>(value)))
        {
            status[site.field] = -1;
            continue;
        }
        fields[site.field] = static_cast<size_t>(value);
        status[site.field] = 1;
    }
    if (locations[1] && locations[3] && image.functionStart(locations[1]) && status[HandleField] == 1 &&
        status[ReportSizeField] == 1 && fields[HandleField] % 8 == 0 && fields[ReportSizeField] % 4 == 0 &&
        fields[ReportSizeField] >= fields[HandleField] + 8)
    {
        a.input = locations[1];
        a.inputHandle = fields[HandleField];
        a.inputReportSize = fields[ReportSizeField];
    }
    bool ctorLinked = false;
    if (locations[4] && displayVtable)
    {
        std::vector<unsigned char> bytes(profile.patterns[4].size);
        if (read(locations[4], bytes.data(), bytes.size()))
            for (size_t i = 0; i + 7 <= bytes.size(); ++i)
                if (bytes[i] == 0x48 && bytes[i + 1] == 0x8d && bytes[i + 2] == 0x05 &&
                    rip(locations[4] + i, 3, 7) == displayVtable)
                    ctorLinked = true;
    }
    bool displayFields = locations[5] && locations[4] && ctorLinked && status[PipeField] == 1 &&
                         status[TypeField] == 1 && status[WidthField] == 1 && status[HeightField] == 1 &&
                         (!newer ? status[PipeBaseField] == 1 : true);
    if (displayFields)
    {
        size_t pipe = fields[PipeField] + (newer ? 0 : fields[PipeBaseField]);
        if (pipe % 8 == 0 && pipe <= 0x10000 && fields[TypeField] % 4 == 0 && fields[WidthField] % 4 == 0 &&
            fields[HeightField] % 4 == 0 && fields[TypeField] != fields[WidthField] &&
            fields[TypeField] != fields[HeightField] && fields[WidthField] != fields[HeightField])
        {
            a.displayPipe = pipe;
            a.displayType = fields[TypeField];
            a.displayWidth = fields[WidthField];
            a.displayHeight = fields[HeightField];
            if (image.functionStart(locations[0]))
                a.rect = locations[0];
            if (locations[2])
            {
                auto begin = rip(locations[2] + (newer ? 7 : 14), 3, 7),
                     end = rip(locations[2] + (newer ? 0 : 7), 3, 7);
                if (end == begin + 8 && image.writable(begin, 24))
                    a.displayVector = begin;
            }
        }
    }
    return a;
}

inline Addresses resolve(const Image &image)
{
    auto display = image.vtable(".?AVCTraktorDisplay@@"), input = image.vtable(".?AVCHIDController@@");
    Addresses result;
    result.displayVtable = display;
    result.inputVtable = input;
    const LayoutProfile *profiles[] = {&OldProfile, &NewProfile};
    unsigned inputMatches = 0, displayMatches = 0;
    for (unsigned group = 0; group < 2; ++group)
    {
        auto &profile = *profiles[group];
        uintptr_t at[6]{};
        for (unsigned i = 0; i < 6; ++i)
            at[i] = image.pattern(profile.patterns[i].bytes, profile.patterns[i].mask, profile.patterns[i].size);
        auto a = profileFields(image, profile, at, display, input, group == 1);
        if (a.input)
        {
            ++inputMatches;
            result.input = a.input;
            result.inputHandle = a.inputHandle;
            result.inputReportSize = a.inputReportSize;
        }
        if (a.displayVector)
        {
            ++displayMatches;
            result.displayVector = a.displayVector;
            result.rect = a.rect;
            result.displayPipe = a.displayPipe;
            result.displayType = a.displayType;
            result.displayWidth = a.displayWidth;
            result.displayHeight = a.displayHeight;
        }
    }
    if (inputMatches != 1)
    {
        result.input = 0;
        result.inputHandle = result.inputReportSize = 0;
    }
    if (displayMatches != 1)
    {
        result.displayVector = result.rect = 0;
        result.displayPipe = result.displayType = result.displayWidth = result.displayHeight = 0;
    }
    return result;
}

// ZH: 精确地址仅作提示，仍须在对应位置解码并验证字段。
// EN: Exact addresses are hints; fields are decoded and checked at those locations.
struct Hints
{
    uintptr_t displayVtable, inputVtable, displayVector, rect, input, vectorReference;
    bool newLayout = false;
};

inline constexpr Hints KnownLocations{0x3f39f20, 0x3f26108, 0x42b2ce8, 0xcdb8c0, 0xbf1d60, 0xcdd5c6};
inline constexpr Hints KnownLocations9644{0x5175c10, 0x51504c8, 0x579e0c8, 0x10a5730, 0xef7030, 0x10a5099, true};
inline constexpr Hints KnownCandidates[]{KnownLocations, KnownLocations9644};

inline bool complete(const Addresses &a)
{
    return a.displayVtable && a.inputVtable && a.displayVector && a.rect && a.input && a.displayPipe && a.inputHandle &&
           a.inputReportSize && a.displayType && a.displayWidth && a.displayHeight;
}

inline Addresses fast(const Image &image, const Hints &hint = KnownLocations)
{
    auto display = image.base + hint.displayVtable, input = image.base + hint.inputVtable;
    if (!image.validVtable(display, ".?AVCTraktorDisplay@@") || !image.validVtable(input, ".?AVCHIDController@@"))
        return {};
    const auto &profile = hint.newLayout ? NewProfile : OldProfile;
    uintptr_t at[6] = {image.base + hint.rect,
                       image.base + hint.input,
                       image.base + hint.vectorReference,
                       image.base + profile.functions[3],
                       image.base + profile.functions[4],
                       image.base + profile.functions[5]};
    for (unsigned i = 0; i < 6; ++i)
        if (!image.matches(at[i], profile.patterns[i].bytes, profile.patterns[i].mask, profile.patterns[i].size))
            return {};
    auto a = profileFields(image, profile, at, display, input, hint.newLayout);
    if (a.displayVector != image.base + hint.displayVector)
        return {};
    return a;
}

struct Timing
{
    bool usedScan = false;
    unsigned long long fastUs = 0, scanUs = 0, totalUs = 0;
};

inline Timing timing;

inline Addresses adaptiveCandidates(HMODULE module, Timing &times, const Hints *hints, size_t count)
{
    using Clock = std::chrono::steady_clock;
    auto start = Clock::now();
    times = {};
    auto elapsed = [](auto at) {
        return static_cast<unsigned long long>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - at).count());
    };
    Image image;
    Addresses result;
    if (image.load(module, false))
        for (size_t i = 0; i < count; ++i)
        {
            result = fast(image, hints[i]);
            if (complete(result))
                break;
        }
    times.fastUs = elapsed(start);
    if (!complete(result))
    {
        // ZH: 保守扫描失败后不得复用只通过部分验证的提示地址。
        // EN: Do not reuse a partially validated hint after the conservative scan fails.
        times.usedScan = true;
        auto scanStart = Clock::now();
        result = {};
        if (image.load(module, true))
            result = resolve(image);
        times.scanUs = elapsed(scanStart);
    }
    times.totalUs = elapsed(start);
    return result;
}

inline Addresses adaptive(HMODULE module, Timing &times)
{
    return adaptiveCandidates(module, times, KnownCandidates, 2);
}

inline Addresses adaptive(HMODULE module, Timing &times, const Hints &hint)
{
    return adaptiveCandidates(module, times, &hint, 1);
}

// ZH: 定位只读执行；任何功能地址不满足独立验证时，调用方只能禁用该功能。
// EN: Resolution is read-only; callers must disable any feature whose address fails independent validation.
inline bool locate()
{
    addresses = {};
    timing = {};
    if (!host())
        return false;
    try
    {
        addresses = adaptive(GetModuleHandleW(nullptr), timing);
        return true;
    }
    catch (...)
    {
        addresses = {};
        return false;
    }
}

inline DWORD WINAPI warningThread(void *ptr)
{
    auto text = static_cast<std::wstring *>(ptr);
    MessageBoxW(nullptr, text->c_str(), L"TK S4MK3 Bridge - Compatibility Warning",
                MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    delete text;
    return 0;
}

inline std::atomic<unsigned> warned{0};

inline void warning(unsigned bit, const std::wstring &path, const char *feature, const wchar_t *message)
{
    if (warned.fetch_or(bit) & bit)
        return;
    if (FILE *f = openLiveLog(path.c_str()))
    {
        SYSTEMTIME t{};
        GetLocalTime(&t);
        liveLogPrintf(
            f,
            "%04u-%02u-%02uT%02u:%02u:%02u WARNING compatibility feature=%s disabled; native VDJ and independent features retained\n",
            t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, feature);
        fclose(f);
    }
    if (!host())
        return;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&warningThread), &module))
        return;
    auto text = new (std::nothrow) std::wstring(message);
    if (!text)
        return;
    HANDLE thread = CreateThread(nullptr, 0, warningThread, text, 0, nullptr);
    if (thread)
        CloseHandle(thread);
    else
        delete text;
}

inline void report(const std::wstring &path)
{
    if (FILE *f = openLiveLog(path.c_str()))
    {
        liveLogPrintf(
            f,
            "compatibility location mode=%s fast_us=%llu scan_us=%llu total_us=%llu display_vtable=%p input_vtable=%p display_vector=%p screen_rect=%p hid_input=%p display_pipe_offset=%zx hid_handle_offset=%zx hid_size_offset=%zx display_type_offset=%zx display_width_offset=%zx display_height_offset=%zx\n",
            timing.usedScan ? "feature_scan" : "known_location_validated", timing.fastUs, timing.scanUs, timing.totalUs,
            reinterpret_cast<void *>(addresses.displayVtable), reinterpret_cast<void *>(addresses.inputVtable),
            reinterpret_cast<void *>(addresses.displayVector), reinterpret_cast<void *>(addresses.rect),
            reinterpret_cast<void *>(addresses.input), addresses.displayPipe, addresses.inputHandle,
            addresses.inputReportSize, addresses.displayType, addresses.displayWidth, addresses.displayHeight);
        fclose(f);
    }
}
}
