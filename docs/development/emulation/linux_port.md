# Running PebbleOS on a Linux watch

The Linux port runs the native PebbleOS firmware as a process alongside the device's existing
userspace. The Galaxy Watch3 variant uses a 260 × 260 round UI, evdev input, Wayland or fbdev
output, and a separate device adapter for sensors, haptics and display power.

Protocol v2 is experimental. Host tests and cross-builds cover the code; on-watch validation of
this protocol revision remains required before release. Deploy the OS and adapter together.

## Build

Install the dependencies and PebbleOS SDK described in the getting-started guide. Put Zig on
`PATH`; the validated compiler for this release candidate is Zig 0.17.0. From the repository:

```sh
cmake -S . -B build-galaxy -GNinja \
  -DBOARD=native_gabbro/galaxy_watch3 \
  -DPEBBLEOS_SDK_ROOT=/path/to/pebbleos-sdk
cmake --build build-galaxy --parallel 4
```

This produces a static AArch64 Linux executable at `build-galaxy/pebbleos` and its
`system_resources.pbpack`. The `native_gabbro/galaxy_watch3_arm32` variant uses the device's
ARM32 glibc loader instead. Both builds need the SDK's ARM tools for third-party app support.

The Git checkout must contain the submodule revisions recorded by the repository. A source
snapshot also carries `patches/nimble-linux.patch`; apply it in the NimBLE submodule before
building. The release bootstrap script checks whether it has already been applied.

```sh
sh tools/linux/bootstrap-native.sh
```

## Runtime

Choose separate directories for sockets and persistent data, and create both. Start the
matching adapter with the same `PBL_RUNTIME_DIR`, then start PebbleOS:

```sh
export PBL_RUNTIME_DIR=/run/pebble
export PBL_DATA_DIR=/var/lib/pebble
mkdir -p "$PBL_RUNTIME_DIR" "$PBL_DATA_DIR"
./pebbleos -r system_resources.pbpack -P galaxy_watch3.profile
```

On the Galaxy Watch3, the built-in defaults use `/opt/usr/home/owner/share/pebble` for both
directories. `-R DIR` overrides the runtime directory; `-P FILE` or `PBL_DEVICE_PROFILE` selects
a device profile. `PBL_RTC_STATE` can override the persistent RTC file directly.

Set `PBL_WAYLAND_DISPLAY` to the device's Wayland socket to use its compositor. Otherwise the
display driver opens `PBL_FB_DEVICE`, defaulting to `/dev/fb0`. Input names and foreground
reclaim delays come from the profile. Hardware access depends on the device account's
permissions; the runtime does not grant privileges.

The OS owns the display timeout and gesture policy. The adapter executes display requests and
reports state. Both display backends keep the ambient clock separate from app frames and
restore the latest app frame on wake. With no adapter, the OS stays interactive and reports
unavailable sensors and controls through the driver APIs.

## Left hand mode

On Galaxy Watch3, choose **Settings → Display → Left Hand Mode → On**, then turn the watch
180 degrees so its buttons sit on the left. The whole display rotates, including apps and
the always-on clock. In the new wearing position, the upper button remains Back and the lower
button remains Select. Bezel Up/Down controls reverse, and touch coordinates and accelerometer
axes follow the display orientation. The preference survives restarts; choose **Off** to restore
the original orientation. A button held during the change keeps its original action until released.

## Tests

```sh
sh tests/linux_adapter/run.sh
sh tests/linux_adapter/run_display_regressions.sh
```

The first command needs a C11 compiler and pthreads. It tests the client against a mock adapter
and runs the conformance checker. The second exercises both display backends with simulated
memory and clocks; it needs C23 and Linux framebuffer headers. On macOS, set `LINUX_HEADERS`
to Zig's `lib/libc/include/any-linux-any` directory.

The firmware's `test_watch_sensor_linux`, `test_hrm_manager` and `test_hrm_manager_linux`
suites cover interpolation, steps and HRM delivery. Run them through `pbl test` or the
corresponding targets in a configured unit-test build.

The `linux-adapter` CI workflow also builds the watch configuration for the Linux runner's CPU,
with simulated Bluetooth and battery backends, and uses `tests/linux_adapter/smoke_os.py` to boot it with
absent, full and partial adapter support. Each case must reach the launcher, save a complete
display frame and respect the declared capabilities. This checks the OS process; hardware
timing and power remain separate tests.
The `test_button_linux` and `test_touch` suites cover rotated controls, releases during an
orientation change and touch coordinates. The display regression script also checks rotated
app frames and both always-on clock styles. Physical wearing and control validation remains
pending while the watch is offline.

## Before a hardware release

Run the checks in the adapter protocol specification: display and gesture latency, long
backlight timeouts, suspend and wake alarms, haptic patterns, HRM quality, step totals and
sensor-hub recovery. Run the adapter conformance checker with `--full`, with the OS stopped.
The connector's README describes how to build and start it without changing boot settings.
