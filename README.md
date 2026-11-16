# mpvNova-libass

A fork of [libass](https://github.com/libass/libass) with subtitle-rendering improvements for Android devices. Used by [mpvNova](https://github.com/Laskco/mpvNova).

## Changes

- Multi-threaded subtitle event rendering.
- Less processing of large off-screen effects.
- Better reuse of cached subtitle bitmaps.
- Faster contour processing on 32-bit ARM.
- Upstream parser and memory-safety fixes.

## Before and After

FLE openings played locally, with the original ASS tracks and fonts. The table
compares the native libraries from mpvNova **2026-10-19** (stock libass 0.17.5)
and **2026-11-15** (this fork) in the same test app. Both builds were speed
compiled. Stats were on; frame drops were counted with recording off.

| Device / mode | Opening | Stock drops | Current drops | Reduction | Video |
| --- | --- | ---: | ---: | ---: | --- |
| NVIDIA Shield TV Pro 2019 / G-Next Copy | Wistoria S01E02 (HEVC) | 573 | 0 | **100%** | [Watch](https://files.catbox.moe/o00uqm.mp4) |
| NVIDIA Shield TV Pro 2019 / G-Next Copy | The Apothecary Diaries S02E02 | 585 | 18 | **96.9%** | [Watch](https://files.catbox.moe/egjdny.mp4) |
| Onn 4K Pro / `gpu` Direct | Wistoria S01E02 (HEVC) | 1,005 | 161 | **84.0%** | [Watch](https://files.catbox.moe/2xkb3d.mp4) |
| Onn 4K Pro / `gpu` Direct | The Apothecary Diaries S02E02 | 1,340 | 481 | **64.1%** | [Watch](https://files.catbox.moe/lx2rqs.mp4) |

Videos were captured separately, so their counters can differ from the table.
These are playback comparisons, not a libass-only benchmark. In a separate run
of the current release on the Shield, G-Next Direct recorded **1** output drop
on the Apothecary opening.

<details>
<summary>Earlier Onn 4K Pro ARM32 comparison</summary>

This test compared two optimized builds, not stock libass against the fork.

| Renderer | Before: dropped frames | After: dropped frames | Reduction |
| --- | ---: | ---: | ---: |
| `gpu-next` | 635 | 483 | **23.9%** |
| `gpu` | 733 | 602 | **17.9%** |

Average subtitle render time also fell from **36.93 ms to 29.49 ms**: **20.2% less time**, or **25.3% higher render throughput**. That's subtitle rendering, not video FPS.

</details>

## Using It in mpvNova

Get the app from [mpvNova releases](https://github.com/Laskco/mpvNova/releases/latest). **ASS performance rendering** is under **Settings > Advanced** and is on by default. Reopen playback after changing it.

Performance mode keeps subtitle styles and animations, but can slightly change rendered edges.

## Building

Autotools and Meson are supported. For Android, see [mpvNova's build scripts](https://github.com/Laskco/mpvNova/tree/main/buildscripts) and [libass build recipe](https://github.com/Laskco/mpvNova/blob/main/buildscripts/scripts/libass.sh).

mpvNova uses a pinned library revision, so changes here need to be rebuilt into the player before they appear in a release.

### Integration Options

| Option | Purpose |
| --- | --- |
| `ASS_EVENT_THREADS` | Compile-time define for multi-threaded rendering. |
| `LIBASS_EXPERIMENTAL_EDGES=1` | Enables performance mode. |
| `LIBASS_RENDER_THREADS=1..4` | Sets the requested render-thread count; `1` selects serial rendering. |

The last two are environment variables, set before creating the renderer. Threaded rendering requires a build with `ASS_EVENT_THREADS` enabled.

## Issues

Report Android playback problems in [mpvNova's issue tracker](https://github.com/Laskco/mpvNova/issues). Include your device, player settings, and a sample or timestamp when possible.

## License

Based on [libass](https://github.com/libass/libass), under the [ISC license](COPYING). Original copyright notices are retained.
