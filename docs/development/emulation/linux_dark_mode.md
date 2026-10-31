# Linux dark-mode verification

Dark mode is enabled through **Settings → Themes → Dark Mode** on Galaxy Watch3 builds.
The default remains Off. System backgrounds use pure black; accent colors, colored Timeline
cards, notification headers, album art and watchfaces retain their own appearance.

## Captured appearance

These are actual 260 × 260 native simulator frames, captured on 2026-10-30.

| Screen | Light | Dark |
| --- | --- | --- |
| Launcher | ![Light launcher](../../_static/images/linux-watch/dark-mode/launcher-light.png) | ![Dark launcher](../../_static/images/linux-watch/dark-mode/launcher-dark.png) |
| Notification | ![Light notification](../../_static/images/linux-watch/dark-mode/notification-light.png) | ![Dark notification](../../_static/images/linux-watch/dark-mode/notification-dark.png) |
| Weather | ![Light weather](../../_static/images/linux-watch/dark-mode/weather-light.png) | ![Dark weather](../../_static/images/linux-watch/dark-mode/weather-dark.png) |

Within the round panel, these samples have 72.4%, 82.3% and 83.2% pure-black pixels in dark mode,
compared with 3.7%, 5.4% and 6.3% in light mode. These are pixel counts, not battery measurements.

| Expanded weather | Globe intro | Revealed globe |
| --- | --- | --- |
| ![Expanded weather](../../_static/images/linux-watch/dark-mode/61_weather_card.png) | ![Globe intro](../../_static/images/linux-watch/dark-mode/62_weather_globe_intro.png) | ![Revealed globe](../../_static/images/linux-watch/dark-mode/63_weather_globe_space.png) |

## Checks

- The host unit suite passes all 414 CTest checks, including graphics image comparisons.
- New theme tests cover rectangular and circular buffers, all 256 pixel values, preserved alpha,
  clipping, neutral icons, toggling, monochrome displays, disabled theming and recovery firmware.
- UI tests check three light/dark round trips with pixel-identical restoration and persistence
  of both On and Off across process restarts.
- Both Linux watch targets build: static AArch64 and ARM32 with the device's glibc loader.
- The manual survey covers launcher rows, settings and nested pickers, notification lists and
  popups, alarms, music, watchfaces, Timeline, Health, Workout, calls and weather with injected
  forecast data. First-use and unavailable-data screens are included.
- The saved pre-change host binary (`46f133f`) and current light-mode build produce 76 matching
  capture names. 71 match exactly. The other five differ only within the animated sloth or
  weather cloud; the remaining pixels match.
- Transition captures sample frames 40 ms apart while opening and closing apps, entering
  Timeline, opening a pin, and entering and revealing the weather globe in both directions.
  The globe test rejects frames with more than 25% white pixels in dark mode, catching the
  white-space inversion seen in the original implementation.
  All 100 sampled forward/reverse reveal frames pass after the fix.

Some survey pages intentionally animate, so `wait_idle` times out and captures their current
frame. Review `_unstable.txt` with the screenshots. A successful capture run alone does not
establish that every screenshot depicts the intended screen. App pages are launched by UUID;
menu navigation uses a 350 ms gap to avoid dropping clicks during scrolling animations.

## Rendering cost

An isolated benchmark of the real color/filter sources, compiled with Apple Clang 21 at `-Os`
on an arm64 Mac, measured a median 0.035 ms per 260 × 260 rectangular filter pass over seven
trials of 2,000 calls. The maximum trial average was 0.042 ms. The light-mode early return was
below the useful resolution of this benchmark. This is a host microbenchmark, not an on-watch
frame-time measurement.

The filter uses a 64-byte lookup table and no per-frame allocation. It is opt-in for the custom
weather windows; the other themed screens draw their chosen colors directly. The globe reveal
themes its line art directly, preserving its black space background and original color pixels
throughout the transition. No periodic theme timer or display refresh is added.

The native port excludes the firmware's Cortex-M profiler. Physical watch frame timing, display
power, battery endurance and wake/suspend behavior remain unmeasured for this branch. The checks
above found no host rendering regression; they do not establish a hardware performance guarantee.

## Reproduce

From the repository root, with the normal build dependencies on `PATH`:

```sh
cmake -S . -B build-dark -GNinja -DBOARD=native_gabbro -DCONFIG_THEMING=y
cmake --build build-dark --parallel 8
pbl test -j 8 --output-on-failure

cd tests/integration
pytest --build-dir=../../build-dark --results-dir=/tmp/theme-tests \
  ui/test_dark_mode.py --dut-scope=function
DARK=1 SURVEY_OUT=/tmp/theme-captures pytest --build-dir=../../build-dark \
  --results-dir=/tmp/theme-survey ui/dark_mode_survey.py --dut-scope=function -n 4
```

`dark_mode_survey.py` is an explicit manual survey and is not collected by the default suite.
Use `DARK=0` and a separate output/results directory for light captures. Keep each concurrent
run's results directory separate. No physical watch was installed or flashed by these checks.
