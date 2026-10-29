// ZH: 灯环颜色解析、硬件色表、缓存目标和指针相位时钟；不负责电机运动。
// EN: Jog-ring color parsing, hardware palette, cache targets and pointer clock; no motor motion control.
#pragma once

// ZH: 把各 Deck 持久颜色转换为 NI 的亮暗颜色对；读取端不轮询原生配色，也不按左右位置强制蓝红。
// EN: Convert each deck's persistent color to NI bright/dim pairs; readers neither poll native colors nor force left/right blue/red.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>

inline bool packedColor(double value, uint32_t &rgb)
{
    if (!std::isfinite(value) || std::floor(value) != value || value < -2147483648.0 || value > 4294967295.0)
        return false;
    rgb = static_cast<uint32_t>(static_cast<int64_t>(value)) & 0xffffff;
    return true;
}

inline uint32_t swapRB(uint32_t v)
{
    return ((v & 255) << 16) | (v & 0xff00) | ((v >> 16) & 255);
}

inline int colorOrder(double red, double green, double blue)
{
    uint32_t r, g, b;
    if (!packedColor(red, r) || !packedColor(green, g) || !packedColor(blue, b))
        return 0;
    if (r == 0xff0000 && g == 0x00ff00 && b == 0x0000ff)
        return 1;
    if (r == 0x0000ff && g == 0x00ff00 && b == 0xff0000)
        return 2;
    return 0;
}

inline bool hexColor(const char *s, uint32_t &value)
{
    if (!s)
        return false;
    while (*s == ' ')
        ++s;
    if (*s == '#')
        ++s;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    else
        return false;
    size_t n = std::strlen(s);
    if (n != 6 && n != 8)
        return false;
    for (size_t i = 0; i < n; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') || (s[i] >= 'A' && s[i] <= 'F')))
            return false;
    value = static_cast<uint32_t>(std::strtoull(s, nullptr, 16)) & 0xffffff;
    return true;
}

inline unsigned niColor(uint32_t rgb)
{
    struct C
    {
        uint32_t rgb;
        unsigned code;
    };

    // ZH: 使用现有 S4 设备定义的离散调色板，不是任意 RGB 输出。
    // EN: Discrete palette from the existing S4 device definition, not full-RGB output.
    const C colors[] = {
        {0, 0},           {0xff0000, 0x06}, {0x800000, 0x04}, {0xcc5e00, 0x0a}, {0x662f00, 0x08}, {0xff7f00, 0x0e},
        {0x804000, 0x0c}, {0xffffc8, 0x12}, {0x7f7f64, 0x10}, {0xffff00, 0x16}, {0x808000, 0x14}, {0x81cc00, 0x1a},
        {0x406600, 0x18}, {0x00ff00, 0x1e}, {0x008000, 0x1c}, {0x00cc49, 0x22}, {0x006624, 0x20}, {0x00ffff, 0x26},
        {0x008080, 0x24}, {0x4a86e6, 0x2a}, {0x254373, 0x28}, {0x0000ff, 0x2e}, {0x000080, 0x2c}, {0x9600ff, 0x32},
        {0x4b007f, 0x30}, {0xad65ff, 0x36}, {0x56327f, 0x34}, {0xff00ff, 0x3a}, {0x800080, 0x38}, {0xe69696, 0x3e},
        {0x734b4b, 0x3c}, {0xcc4761, 0x42}, {0x662330, 0x40}, {0xffffff, 0x46}, {0x7f7f7f, 0x44}, {0xd8d8d8, 0x46}};
    unsigned best = 0;
    long distance = 0x7fffffff;
    for (const auto &c : colors)
    {
        long r = long((rgb >> 16) & 255) - long((c.rgb >> 16) & 255),
             g = long((rgb >> 8) & 255) - long((c.rgb >> 8) & 255), b = long(rgb & 255) - long(c.rgb & 255);
        long d = r * r + g * g + b * b;
        if (d < distance)
        {
            distance = d;
            best = c.code;
        }
    }
    return best;
}

inline bool vdjColor(const char *text, uint32_t &rgb)
{
    if (!text)
        return false;
    char name[64]{};
    size_t n = std::strlen(text);
    while (n && text[n - 1] == ' ')
        --n;
    while (n && *text == ' ')
    {
        ++text;
        --n;
    }
    if (n >= sizeof(name))
        return false;
    for (size_t i = 0; i < n; ++i)
        name[i] = (text[i] >= 'A' && text[i] <= 'Z') ? char(text[i] + ('a' - 'A')) : text[i];

    struct Named
    {
        const char *name;
        uint32_t rgb;
    };

    const Named names[] = {{"black", 0},
                           {"white", 0xffffff},
                           {"red", 0xff0000},
                           {"green", 0x00ff00},
                           {"blue", 0x0000ff},
                           {"orange", 0xff7f00},
                           {"yellow", 0xffff00},
                           {"cyan", 0x00ffff},
                           {"marine", 0x4a86e6},
                           {"magenta", 0xff00ff},
                           {"violet", 0x9600ff},
                           {"beige", 0xffffc8},
                           {"pink", 0xe69696},
                           {"gray", 0x7f7f7f},
                           {"grey", 0x7f7f7f},
                           {"lightgray", 0xd8d8d8},
                           {"darkgray", 0x646464},
                           {"darkred", 0x800000},
                           {"darkgreen", 0x008000},
                           {"darkblue", 0x000080},
                           {"darkorange", 0x804000},
                           {"darkyellow", 0x808000},
                           {"darkcyan", 0x008080},
                           {"darkmagenta", 0x800080}};
    for (const auto &c : names)
        if (std::strcmp(name, c.name) == 0)
        {
            rgb = c.rgb;
            return true;
        }
    return hexColor(name, rgb);
}

template <class Numeric, class Text>
bool configuredDeckColor(unsigned deck, int order, Numeric numeric, Text text, uint32_t &rgb)
{
    if (deck < 1 || deck > 4)
        return false;
    const std::string query = "deck " + std::to_string(deck) + " get_var '@s4mk3_deck_color'";
    // ZH: 保存的颜色名称或 HEX 是文本；优先读取数值会把有效文本
    // EN: A stored name/HEX is text. Reading numeric first can turn a valid text
    // ZH: 变量变成零，导致灯环被错误关闭。
    // EN: variable into zero, which would incorrectly extinguish the ring.
    std::string value;
    if (text(query.c_str(), value))
        return vdjColor(value.c_str(), rgb);
    double packed = 0;
    if (!order || !numeric(query.c_str(), packed) || !packedColor(packed, rgb))
        return false;
    if (order == 2)
        rgb = swapRB(rgb);
    return true;
}

// ZH: 生成灯环缓存状态；只在需要时更新模式和颜色，保持设备侧角度与播放状态一致。
// EN: Generate ring cache state; update mode/color only as needed and keep device angle aligned with playback.

#include "bridge_ipc.h"

#include <cstddef>

namespace ring18
{
struct Cache
{
    DWORD mode;
    BYTE color, reserved;
    WORD angle;
};

static_assert(sizeof(Cache) == 8 && offsetof(Cache, color) == 4 && offsetof(Cache, angle) == 6);

inline bool validCache(const Cache &c)
{
    return (c.mode == 0 || c.mode == 2 || c.mode == 3) && c.angle < 2880;
}

inline Cache target(const phase18::Frame &f, unsigned side, const Cache &current)
{
    Cache out = current;
    if (!(f.valid & (1u << side)) || !(f.facts[side] & phase18::Loaded))
    {
        out.mode = 0;
        return out;
    }
    int angle = phase18::angle(f, side);
    if (angle < 0)
    {
        out.mode = 0;
        return out;
    }
    out.angle = static_cast<WORD>(angle);
    out.color = static_cast<BYTE>(niColor(f.rgb[side]));
    DWORD flags = f.facts[side];
    bool touched = (flags & phase18::Touched) || (f.padding & (1u << (side + 2)));
    out.mode =
        !touched && !(flags & phase18::LineIn) && (flags & phase18::Audible) && (flags & phase18::NearEnd) ? 3u : 2u;
    return out;
}

// ZH: 单次 8 字节缓存写入只能缩短竞争窗口，不能保证跨进程原子性。
// EN: A single 8-byte cache write minimizes the window but is NOT a cross-process atomicity guarantee.
template <class Read, class Write> bool update(const phase18::Frame &frame, unsigned side, Read read, Write write)
{
    Cache before{};
    if (!read(side, before) || !validCache(before))
        return false;
    Cache next = target(frame, side, before);
    return !memcmp(&before, &next, sizeof(next)) || write(side, next);
}
}

// ZH: 解码 Report 3 实体角度；混合模式的自动播放相位由下方 Clock 单独计算。
// EN: Decode physical Report 3 angles; Clock below separately computes hybrid automatic playback phase.
namespace physicalJog
{
inline bool decode(const BYTE *payload, size_t length, WORD (&angles)[2])
{
    if (!payload || length < 45)
        return false;
    WORD left = 0, right = 0;
    memcpy(&left, payload + 15, 2);
    memcpy(&right, payload + 43, 2);
    if (left >= 2880 || right >= 2880)
        return false;
    angles[0] = left;
    angles[1] = right;
    return true;
}

inline void apply(phase18::Frame &frame, const WORD (&angles)[2], bool valid)
{
    if (!(frame.padding & 3u))
        return;
    // ZH: 尚未收到合法硬件角度时不伪造指针；其余功能继续使用有效 Deck 状态。
    // EN: Do not invent a pointer before valid hardware angles arrive; retain deck validity for other functions.
    frame.left = valid ? double(angles[0]) / 2880.0 : std::nan("");
    frame.right = valid ? double(angles[1]) / 2880.0 : std::nan("");
}
}

namespace physicalJog
{
// ZH: JOG 模式累加自动播放相位与手动位移；TT 模式只取实体角度，避免重复旋转。
// EN: JOG adds automatic playback phase and manual displacement; TT uses hardware angles only.
class Clock
{
    // ZH: 状态按左右实体侧保存，不因切 Deck 重置；模式、连接代次或长采样中断会重新建立基线。
    // EN: Keep state per physical side across deck changes; rebase on mode/generation changes or long sampling gaps.
    double phase[2]{}, previous[2]{};
    bool known[2]{}, running[2]{};
    ULONGLONG tick = 0, generation = 0;
    DWORD selected = 99;

    static double wrap(double x)
    {
        return x - std::floor(x);
    }

  public:
    void apply(phase18::Frame &f, ULONGLONG now)
    {
        DWORD mode = f.padding & 3u;
        bool reset = mode != selected || generation != f.generation;
        bool timely = !reset && tick && now >= tick && now - tick <= 250;
        double dt = timely ? double(now - tick) / 1000 : 0;
        selected = mode;
        generation = f.generation;
        tick = now;
        if (!mode)
        {
            known[0] = known[1] = false;
            running[0] = running[1] = false;
            return;
        }
        for (unsigned side = 0; side < 2; ++side)
        {
            double physical = side ? f.right : f.left;
            DWORD flags = f.facts[side];
            bool valid = (f.valid & (1u << side)) && std::isfinite(physical) && physical >= 0 && physical < 1;
            if (!valid)
            {
                known[side] = running[side] = false;
                continue;
            }
            bool initialize = reset || !known[side] || !timely;
            double delta = initialize ? 0 : physical - previous[side];
            if (delta > .5)
                delta -= 1;
            else if (delta < -.5)
                delta += 1;
            previous[side] = physical;
            known[side] = true;
            if (initialize)
                phase[side] = physical;
            bool touched = (flags & phase18::Touched) || (f.padding & (1u << (side + 2)));
            double rpm = f.speedMode == 3 ? f.bpm[side] / 4 : (f.speedMode == 1 ? 100.0 / 3.0 : 45.0) * f.tempo[side];
            bool run = (flags & phase18::Loaded) && !(flags & phase18::LineIn) && (flags & phase18::Playing) &&
                       !touched && !(f.padding & (1u << (side + 4))) && std::isfinite(rpm) && rpm > 0 && rpm <= 75;
            if (flags & phase18::TT)
            {
                phase[side] = physical;
                running[side] = false;
            }
            else
            {
                phase[side] = wrap(phase[side] + delta + (run && running[side] ? dt * rpm / 60 : 0));
                running[side] = run;
            }
            if (side)
                f.right = phase[side];
            else
                f.left = phase[side];
        }
    }
};
}
