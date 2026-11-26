# mpvNova-libass

A fork of [libass](https://github.com/libass/libass) with subtitle-rendering improvements for Android devices. Used by [mpvNova](https://github.com/Laskco/mpvNova) and [Nuvio](https://github.com/NuvioMedia/NuvioTV).

<p align="center">
  <a href="https://github.com/Laskco/mpvNova"><img src="https://raw.githubusercontent.com/Laskco/mpvNova/main/fastlane/metadata/android/en-US/images/tvBanner.png" width="48%" alt="mpvNova" /></a>
  <a href="https://github.com/NuvioMedia/NuvioTV"><img src="https://raw.githubusercontent.com/NuvioMedia/NuvioTV/main/app/src/main/res/drawable-nodpi/tv_banner.png" width="48%" alt="Nuvio" /></a>
</p>

## Changes

- Multi-threaded subtitle event rendering and sliced blur execution.
- Transparent-plateau and static-plateau event caching to skip redundant frame work.
- Faster vector-clip compositing, shadow reuse, small-blur fast paths, and less processing of large off-screen effects.
- Per-glyph cached outline LOD and contour simplification for Performance (`1`) and Ultra (`2`) edge modes.
- Age-based stale cache trimming above a warm floor to keep RAM bounded after heavy openings.
- Upstream parser and memory-safety fixes.

## Before and After

Full-length openings played locally with original ASS tracks and embedded fonts (`vo=gpu-next`, `video-sync=audio`, `interpolation=no`, `sub-ass-override=no`). Every build was speed compiled (`cmd package compile -m speed`).

- **Stock:** `mpv-android` 2026-09-17 (upstream libass 0.17.5; `mediacodec-copy` on Shield since upstream lacks the `libplacebo` update for Direct, `mediacodec` Direct on Onn).
- **2026-11-22:** mpvNova 2026-11-22 (`b620927`, Direct).
- **Current:** mpvNova 2026-11-26 (`6a7c6c9`, Direct).

Values in **Mode 0/1/2** columns correspond to **Exact / Performance / Ultra**. **Saved** compares Current Mode 1 against Stock.

### Dropped Frames

| Device | Opening | Stock | 2026-11-22 (Mode 0/1/2) | Current (Mode 0/1/2) | Saved |
| --- | --- | ---: | ---: | ---: | ---: |
| Shield TV Pro 2019 | [FLE] Apothecary S02E02 | 572 | **0**&nbsp;/&nbsp;**0**&nbsp;/&nbsp;**0** | **0**&nbsp;/&nbsp;**0**&nbsp;/&nbsp;**0** | **100%** |
| Shield TV Pro 2019 | [FLE] Wistoria S01E02 | 559 | **0**&nbsp;/&nbsp;**0**&nbsp;/&nbsp;**0** | **0**&nbsp;/&nbsp;**0**&nbsp;/&nbsp;**0** | **100%** |
| Shield TV Pro 2019 | [Kawatare] Fate/strange Fake S01E02 | 509 | **0**&nbsp;/&nbsp;**0**&nbsp;/&nbsp;**0** | **0**&nbsp;/&nbsp;**0**&nbsp;/&nbsp;**0** | **100%** |
| Onn 4K Pro | [FLE] Apothecary S02E02 | 1,355 | 301&nbsp;/&nbsp;166&nbsp;/&nbsp;120 | 287&nbsp;/&nbsp;**0**&nbsp;/&nbsp;**1** | **100%** |
| Onn 4K Pro | [FLE] Wistoria S01E02 | 969 | 228&nbsp;/&nbsp;3&nbsp;/&nbsp;0 | 218&nbsp;/&nbsp;**0**&nbsp;/&nbsp;**0** | **100%** |
| Onn 4K Pro | [Kawatare] Fate/strange Fake S01E02 | 1,373 | 235&nbsp;/&nbsp;206&nbsp;/&nbsp;19 | 76&nbsp;/&nbsp;**0**&nbsp;/&nbsp;**0** | **100%** |

### Peak A/V Drift (`max |avsync|`)

| Device | Opening | Stock | 2026-11-22 (Mode 0/1/2) | Current (Mode 0/1/2) |
| --- | --- | ---: | ---: | ---: |
| Shield TV Pro 2019 | [FLE] Apothecary S02E02 | 0.119s | 0.001s&nbsp;/&nbsp;0.001s&nbsp;/&nbsp;0.001s | **0.001s**&nbsp;/&nbsp;**0.002s**&nbsp;/&nbsp;**0.001s** |
| Shield TV Pro 2019 | [FLE] Wistoria S01E02 | 0.239s | 0.001s&nbsp;/&nbsp;0.000s&nbsp;/&nbsp;0.000s | **0.002s**&nbsp;/&nbsp;**0.000s**&nbsp;/&nbsp;**0.000s** |
| Shield TV Pro 2019 | [Kawatare] Fate/strange Fake S01E02 | 0.146s | 0.022s&nbsp;/&nbsp;0.009s&nbsp;/&nbsp;0.017s | **0.020s**&nbsp;/&nbsp;**0.011s**&nbsp;/&nbsp;**0.022s** |
| Onn 4K Pro | [FLE] Apothecary S02E02 | 0.515s | 0.141s&nbsp;/&nbsp;0.109s&nbsp;/&nbsp;0.085s | 0.117s&nbsp;/&nbsp;**0.003s**&nbsp;/&nbsp;**0.003s** |
| Onn 4K Pro | [FLE] Wistoria S01E02 | 0.433s | 0.096s&nbsp;/&nbsp;0.033s&nbsp;/&nbsp;0.029s | 0.097s&nbsp;/&nbsp;**0.003s**&nbsp;/&nbsp;**0.012s** |
| Onn 4K Pro | [Kawatare] Fate/strange Fake S01E02 | 0.295s | 0.089s&nbsp;/&nbsp;0.089s&nbsp;/&nbsp;0.084s | 0.072s&nbsp;/&nbsp;**0.036s**&nbsp;/&nbsp;**0.030s** |

All files were tested locally, so network streaming will have different results due to additional network, TLS, and buffering overhead. These are full playback comparisons, not a libass-only benchmark. Even in **Mode 0 (Exact)** with zero visual approximation, current `master` cuts drops by **78.8%** on [FLE] Apothecary (`1,355 -> 287`), **77.5%** on [FLE] Wistoria (`969 -> 218`), and **94.5%** on [Kawatare] Fate (`1,373 -> 76`) on the Onn 4K Pro, and hits **0 drops** across all three openings on the Shield TV Pro 2019.

## Using It in mpvNova

Get the app from [mpvNova releases](https://github.com/Laskco/mpvNova/releases/latest). **ASS rendering mode** is under **Settings > Advanced** and defaults to **Performance (`1`)**. Reopen playback after changing it.

- **Exact (`0`):** Pixel-identical output (full rasterizer precision, full subpixel positioning, exact Gaussian blur selection, and exact `0xFF` alpha only) with all lossless speedups (multi-threading, plateau caching, vector-clip bounds, shadow reuse, NEON SIMD, and stale cache trimming).
- **Performance (`1`):** Keeps subtitle styles and animations intact with subtle approximations for smooth playback on lower-power Android TV hardware: `2x` curve tolerance, per-glyph outline LOD and contour clipping, half-pixel (`0.5 px`) glyph snapping, a narrow small-blur fast path, and skipping nearly invisible `1/255` (`0xFE`) fade tails.
- **Ultra (`2`):** Stronger approximations for extreme particle and vector-drawing scenes: `4x` curve tolerance, stronger outline LOD, whole-pixel (`1 px`) glyph and vector-clip snapping, and a wider small-blur fast path.

## Building

Autotools and Meson are supported. For Android, see [mpvNova's build scripts](https://github.com/Laskco/mpvNova/tree/main/buildscripts) and [libass build recipe](https://github.com/Laskco/mpvNova/blob/main/buildscripts/scripts/libass.sh).

mpvNova uses a pinned library revision, so changes here need to be rebuilt into the player before they appear in a release.

### Integration Options

| Option | Purpose |
| --- | --- |
| `ASS_EVENT_THREADS` | Compile-time define for multi-threaded rendering. |
| `LIBASS_EXPERIMENTAL_EDGES=0\|1\|2` | Selects Exact (`0`), Performance (`1`), or Ultra (`2`) rendering mode. |
| `LIBASS_RENDER_THREADS=1..4` | Sets the requested render-thread count; `1` selects serial rendering. |

The last two are environment variables, set before creating the renderer. Threaded rendering requires a build with `ASS_EVENT_THREADS` enabled.

### Companion `mpv` Subtitle Patches

When integrating this fork with `mpv`, `mpvNova` also applies two optional `mpv`-side subtitle patches in [`buildscripts/patches`](https://github.com/Laskco/mpvNova/tree/main/buildscripts/patches):

- [`mpv-ass-fade-upload.patch`](https://github.com/Laskco/mpvNova/blob/main/buildscripts/patches/mpv-ass-fade-upload.patch): reuses the packed subtitle atlas instead of re-uploading unchanged pixels during fade, color, and position animations.
- [`mpv-sub-atlas-shrink.patch`](https://github.com/Laskco/mpvNova/blob/main/buildscripts/patches/mpv-sub-atlas-shrink.patch): shrinks oversized CPU and GPU subtitle atlases back down after heavy openings finish.

## Issues

Report Android playback problems in [mpvNova's issue tracker](https://github.com/Laskco/mpvNova/issues). Include your device, player settings, and a sample or timestamp when possible.

## License

Based on [libass](https://github.com/libass/libass), under the [ISC license](COPYING). Original copyright notices are retained.
