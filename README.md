# mpvNova-libass

A fork of [libass](https://github.com/libass/libass) with subtitle-rendering improvements for Android devices. Used by [mpvNova](https://github.com/Laskco/mpvNova).

## Changes

- Multi-threaded subtitle event rendering.
- Less processing of large off-screen effects.
- Better reuse of cached subtitle bitmaps.
- Faster contour processing on 32-bit ARM.
- Upstream parser and memory-safety fixes.

## Before and After

### NVIDIA Shield Pro 2019

FLE openings, comparing stock libass with this fork. Stats were visible during recording.

| Opening | Before: dropped frames | After: dropped frames | Reduction |
| --- | ---: | ---: | ---: |
| Wistoria - Wand and Sword, S01E02 | 576 | 0 | **100%** |
| The Apothecary Diaries, S02E01 | 618 | 64 | **89.6%** |

**Wistoria S01E02**

https://github.com/user-attachments/assets/97908d78-f012-4c14-a514-6bd4c5242430

[Original video](https://files.catbox.moe/j2lhfx.mp4)

**The Apothecary Diaries S02E01**

https://github.com/user-attachments/assets/8000c564-a228-43df-8664-5a0261c73725

[Original video](https://files.catbox.moe/9v5tds.mp4)

### Onn 4K Pro

The Apothecary Diaries S02E01 opening, comparing the latest ARM32 optimization with an earlier optimized build. Same clip and settings, with Stats visible.

| Renderer | Before: dropped frames | After: dropped frames | Reduction |
| --- | ---: | ---: | ---: |
| `gpu-next` | 635 | 483 | **23.9%** |
| `gpu` | 733 | 602 | **17.9%** |

Average subtitle render time also fell from **36.93 ms to 29.49 ms**: **20.2% less time**, or **25.3% higher render throughput**. That's subtitle rendering, not video FPS.

These numbers are from the clips above. Results vary by file and device, and demanding subtitles can still cause dropped frames.

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
