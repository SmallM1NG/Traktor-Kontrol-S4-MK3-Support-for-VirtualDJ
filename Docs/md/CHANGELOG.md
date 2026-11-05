# Changelog

**English** · [中文](CHANGELOG_zh.md)

## v0.1.1

### 🤨 What's Changed

#### ✨ Added

- Added the experimental **TTModeReverse** setting. When enabled with TT mode on, the motor can rotate in reverse to follow the Deck's reverse / Dump state.
- Added the **TITLE SCROLL ON/OFF** PAD setting to control long-title scrolling on pages 1–6, reserving space when the Stems indicator is visible.
- Added the **BROWSER 1/2 LINES** PAD setting to switch the custom browser between single-line and two-line layouts.
- Added compatibility adaptation for more VirtualDJ versions, retaining validated address lookup and automatic feature scanning. These versions have not undergone full testing; see [Supported Versions](SUPPORTED_VERSIONS.md).

#### ⚡ Improved

- Optimized processing in screen output takeover mode to send only changed regions, allowing the screens to update at close to 60 frames per second.
- Adjusted the custom browser's column widths, spacing and alignment to prevent BPM and track length from being clipped.
- Improved exit cleanup by saving the online device serial number and using it to reconnect for cleanup if the original connection is lost.
- Updated Traktor Style ring behavior: use the selected base speed and Tempo during playback, follow manual movement while touched, and resume automatic rotation after release.
- Renamed controller settings to JogLEDRingMode and TTModeBaseSpeed, and ring modes to Traktor Style and Pointer Position.
- Standardized boolean controller options as yes / no and moved experimental notices to descriptions.
- Reordered Settings PAD pages to TT/COLOR → Screen Switch → Screen Style.
- Adjusted the defaults for some settings.

#### 🐛 Fixed

- Fixed the precomputed Stems indicator disappearing at negative playback positions on pages 1–6.
- Fixed cases where end-of-track warning flashing continued after VirtualDJ exited because its state was not successfully cleared.

Completely remove the old version's files and reinstall the latest version. For usage instructions and remaining limitations, see the [README](../../README.md).

## v0.1.0

- First public version.
- For detailed instructions, please check the [README](../../README.md).

> [!WARNING]
> **Known Issues**
>
> 1. **Jog rim nudging in TT mode:** With TT enabled, touching the side of the platter cannot temporarily speed up or slow down playback. I tried several approaches, but each had stability issues. With limited energy left to keep trying, I have put this aside for now.
> 2. **HapticHotcues point detection:** Only Hotcue / Saved Loop points numbered 1–8 are supported; marker-only or hidden points are not supported. A Hotcue set while a Loop is active behaves like a Saved Loop, but its Loop length cannot be read correctly. Restart VirtualDJ and let it automatically become a Saved Loop before its length can be read correctly.
> 3. **Motor speed limit:** The motor speed limit is <strong>75 RPM (300 BPM ÷ 4)</strong> and applies to all speed modes. This limit is imposed because it is unclear whether higher speeds could damage the motor. If the speed about to be sent to the motor exceeds the limit, it is rejected and the motor stops immediately. Once the speed returns within the limit, rotation resumes automatically while TT mode is enabled and the track is playing.
