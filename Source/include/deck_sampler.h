// ZH: 通过SDK查询实际Deck的播放事实、速度、颜色和盘面位置，生成发布帧所需快照。
// EN: Sample actual-deck playback facts, speed, color and platter position through the SDK for publishing snapshots.
#pragma once

// ZH: 播放/触摸等状态的位标志定义。DLL 与 EXE 必须使用同一套编码，不能自行改变位语义。
// EN: Bit flags for playback, touch and related state. DLL and EXE must share the same encoding and bit semantics.

#include <cmath>

template <class Query> int readDeckPlay(bool right, Query query, bool &valid)
{
    const char *dq = right ? "deck right get_deck" : "deck left get_deck";
    const char *pq = right ? "deck right play" : "deck left play";
    double before = 0, play = 0, after = 0;
    valid = query(dq, before) && query(pq, play) && query(dq, after);
    valid = valid && std::isfinite(before) && std::isfinite(after) && std::isfinite(play) && before == after &&
            before >= 1 && before <= 4 && std::floor(before) == before && (play == 0 || play == 1);
    return valid ? static_cast<int>(play) : 0;
}

// ZH: 通过 SDK 读取左右实际 Deck 的播放事实、速度、颜色和盘面状态，构造 DLL 发布帧。
// EN: Read playback facts, speed, color and platter state for the actual left/right decks through the SDK to construct DLL frames.

#include "bridge_ipc.h"
#include <string>

namespace sample18
{
struct Sample
{
    DWORD deck = 0, facts = 0, rgb = 0;
    double rotation = 0, bpm = 0, tempo = 0;
    bool valid = false;
};

inline bool deckNumber(double v)
{
    return std::isfinite(v) && v >= 1 && v <= 4 && std::floor(v) == v;
}

template <class Query, class Color> Sample read(bool right, Query query, Color color)
{
    Sample out;
    double before = 0, after = 0;
    const char *alias = right ? "deck right get_deck" : "deck left get_deck";
    if (!query(alias, before) || !deckNumber(before))
        return out;
    const DWORD deck = static_cast<DWORD>(before);
    out.deck = deck;
    const std::string prefix = "deck " + std::to_string(deck) + " ";
    auto get = [&](const char *verb, double &value) {
        return query((prefix + verb).c_str(), value);
    };
    auto boolean = [](double v) {
        return v == 0 || v == 1;
    };
    double loaded = 0, linein = 0, audible = 0, nearEnd = 0, rotation = 0, tt = 0, playing = 0, touch = 0;
    DWORD rgb = 0;
    bool ok = get("loaded", loaded) && boolean(loaded);
    if (ok && loaded == 1)
    {
        ok = get("get_rotation", rotation) && get("linein", linein) && get("is_audible", audible) &&
             get("songpos_remain 30000ms", nearEnd) && color(deck, rgb) && get("var '@s4mk3_tt_mode'", tt) &&
             get("play", playing) && get("var 's4mk3_tt_touch'", touch);
        ok = ok && std::isfinite(rotation) && rotation >= 0 && rotation <= 1 && boolean(linein) && boolean(audible) &&
             boolean(nearEnd) && rgb <= 0xffffff && boolean(tt) && boolean(playing) && boolean(touch);
    }
    if (ok && loaded == 1)
    {
        double bpm = 0;
        if (get("get_bpm", bpm) && std::isfinite(bpm) && bpm > 0)
            out.bpm = bpm;
    }
    if (ok && loaded == 1)
    {
        double tempo = 0;
        if (get("get_pitch_value", tempo) && std::isfinite(tempo) && tempo > 0)
            out.tempo = tempo / 100.0;
    } // ZH: get_pitch_value 返回百分比，100 对应 1 倍速度。 | EN: get_pitch_value returns percent (100 = 1x).
    ok = ok && query(alias, after) && after == before;
    if (ok)
    {
        out.valid = true;
        out.rotation = rotation == 1 ? 0 : rotation;
        out.rgb = rgb;
        out.facts = (loaded == 1 ? phase18::Loaded : 0u) | (linein == 1 ? phase18::LineIn : 0u) |
                    (audible == 1 ? phase18::Audible : 0u) | (nearEnd == 1 ? phase18::NearEnd : 0u) |
                    (tt == 1 ? phase18::TT : 0u) | (playing == 1 ? phase18::Playing : 0u) |
                    (touch == 1 ? phase18::Touched : 0u);
    }
    return out;
}
}
