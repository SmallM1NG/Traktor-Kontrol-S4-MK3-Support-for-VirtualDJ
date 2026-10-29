# Changelog

**English** · [中文](CHANGELOG_zh.md)

## v0.1.0

- First public version.
- For detailed instructions, please check the [README](../../README.md).

> [!WARNING]
> **Known Issues**
>
> 1. **Jog rim nudging in TT mode:** With TT enabled, touching the side of the platter cannot temporarily speed up or slow down playback. I tried several approaches, but each had stability issues. With limited energy left to keep trying, I have put this aside for now.
> 2. **HapticHotcues point detection:** Only Hotcue / Saved Loop points numbered 1–8 are supported; marker-only or hidden points are not supported. A Hotcue set while a Loop is active behaves like a Saved Loop, but its Loop length cannot be read correctly. Restart VirtualDJ and let it automatically become a Saved Loop before its length can be read correctly.
> 3. **Motor speed limit:** The motor speed limit is <strong>75 RPM (300 BPM ÷ 4)</strong> and applies to all speed modes. This limit is imposed because it is unclear whether higher speeds could damage the motor. If the speed about to be sent to the motor exceeds the limit, it is rejected and the motor stops immediately. Once the speed returns within the limit, rotation resumes automatically while TT mode is enabled and the track is playing.
