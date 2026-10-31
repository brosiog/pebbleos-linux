# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

"""Manual screenshot survey; invoke explicitly and review the captured frames."""

import os
import time

import pytest
from harness.errors import WatchTimeout
from harness.helpers import notifications
from harness.helpers.ui import Button

OUT = os.environ.get("SURVEY_OUT", "/tmp/dm-survey")
DARK = os.environ.get("DARK") == "1"
NOW = 1767261600
THEMES_ROW = 9

pytestmark = [pytest.mark.platforms("gabbro"), pytest.mark.device_types("native")]


def shot(ui, name):
    try:
        img = ui.wait_idle(timeout=8)
    except WatchTimeout as e:
        with open(os.path.join(OUT, "_unstable.txt"), "a") as f:
            f.write(f"{name}: {e}\n")
        img = ui.screenshot()
    img.save(os.path.join(OUT, f"{name}.png"))
    return img


def open_settings(ui):
    ui.go_home()
    ui.launch_app("07e0d9cb-8957-4bf7-9d42-35bf47caadfe")  # Settings
    time.sleep(1.5)
    ui.wait_idle()


def set_dark(dut, ui):
    if not dut.build.config.get("CONFIG_THEMING"):
        assert not DARK, "Dark captures require CONFIG_THEMING=y"
        return
    open_settings(ui)
    ui.press(Button.DOWN, presses=THEMES_ROW, gap_ms=350)
    ui.wait_idle()
    ui.press(Button.SELECT)
    img = ui.wait_idle()
    # Below the two Themes rows the list background shows.
    if (img.getpixel((130, 215)) == (0, 0, 0)) != DARK:
        ui.press(Button.SELECT)
        img = ui.wait_idle()
    assert (img.getpixel((130, 215)) == (0, 0, 0)) == DARK
    ui.go_home()


@pytest.fixture(autouse=True)
def mode(dut, ui):
    os.makedirs(OUT, exist_ok=True)
    ui.set_time(NOW)
    set_dark(dut, ui)


@pytest.mark.parametrize("row", range(1, 12))
def test_settings(dut, ui, row):
    open_settings(ui)
    ui.press(Button.DOWN, presses=row, gap_ms=350)
    shot(ui, f"3{row:02d}_a_top")
    ui.press(Button.SELECT)
    shot(ui, f"3{row:02d}_b_sub")
    for i in range(3):
        ui.press(Button.DOWN)
        shot(ui, f"3{row:02d}_c{i}_sub")
    ui.press(Button.SELECT)
    shot(ui, f"3{row:02d}_d_nested")
    ui.press(Button.DOWN)
    shot(ui, f"3{row:02d}_e_nested")


LAUNCHER = [
    "Settings",
    "Music",
    "Notifications",
    "Alarms",
    "Watchfaces",
    "Weather",
    "Workout",
    "Health",
    "Timeline",
]

APP_UUIDS = [
    "07e0d9cb-8957-4bf7-9d42-35bf47caadfe",
    "1f03293d-47af-4f28-b960-f2b02a6dd757",
    "b2cae818-10f8-46df-ad2b-98ad2254a3c1",
    "67a32d95-ef69-46d4-a0b9-854cc62f97f9",
    "18e443ce-38fd-47c8-84d5-6d0c775fbe55",
    "61b22bc8-1e29-460d-a236-3fe409a439ff",
    "fef82c82-7176-4e22-88de-35a3fc18d43f",
    "36d8c6ed-4c83-4fa1-a9e2-8f12dc941f8c",
    "426ccd53-b380-4d83-8d06-9893de3477ce",
]


def open_app(ui, index):
    ui.go_home()
    ui.launch_app(APP_UUIDS[index])
    time.sleep(1)
    ui.wait_idle()


def open_launcher_row(ui, index):
    ui.go_home()
    ui.press(Button.SELECT)
    ui.wait_idle()
    ui.press(Button.UP, presses=12, gap_ms=350)
    ui.wait_idle()
    if index:
        ui.press(Button.DOWN, presses=index, gap_ms=350)
        ui.wait_idle()


def test_launcher(dut, ui):
    open_launcher_row(ui, 0)
    shot(ui, "10_launcher_0")
    for i in (2, 5, 8):
        open_launcher_row(ui, i)
        shot(ui, f"10_launcher_{i}")


@pytest.mark.parametrize("index", range(1, 9))
def test_app(dut, ui, index):
    if LAUNCHER[index] == "Notifications":
        notifications.clear(dut)
        notifications.send(
            dut,
            "Running 10 min late, see you at the cafe",
            sender="Anna",
            timestamp=NOW,
        )
        time.sleep(2)
        shot(ui, "29_notif_popup")
        ui.press(Button.SELECT)
        shot(ui, "29_notif_popup_actions")
        ui.press(Button.BACK)
        ui.press(Button.BACK)
    open_app(ui, index)
    name = f"2{index}_{LAUNCHER[index]}"
    shot(ui, f"{name}_a")
    ui.press(Button.DOWN)
    shot(ui, f"{name}_b")
    ui.press(Button.SELECT)
    shot(ui, f"{name}_c")
    ui.press(Button.DOWN)
    shot(ui, f"{name}_d")
    ui.press(Button.SELECT)
    shot(ui, f"{name}_e")


import struct
import uuid as _uuid

from harness.helpers.notifications import _attribute, _blobdb, _Key


def _res(attr_id, value):
    return struct.pack("<BHI", attr_id, 4, value)


def insert_pin(dut, when, title, location, layout=2, duration=60):
    attrs = [
        _attribute(1, title),
        _attribute(11, location),
        _res(10, 0x80000015),
        _attribute(3, "Bring the slides and the demo watch"),
    ]
    payload = b"".join(attrs)
    pin_id = _uuid.uuid4()
    header = struct.pack(
        "<16s16sIHBBBBHBB",
        pin_id.bytes,
        _uuid.UUID(int=0).bytes,
        when,
        duration,
        2,
        1,
        0,
        layout,
        len(payload),
        len(attrs),
        0,
    )
    _blobdb(dut.protocol, "insert", 0x01, pin_id, header + payload)


def test_timeline(dut, ui):
    insert_pin(dut, NOW + 1800, "Design review", "Room 4B")
    insert_pin(dut, NOW + 7200, "Lunch with Sam", "Cafe on 5th", layout=1)
    insert_pin(dut, NOW - 3600, "Standup", "Zoom")
    time.sleep(1)
    open_app(ui, 8)
    shot(ui, "40_timeline_a")
    ui.press(Button.DOWN)
    shot(ui, "40_timeline_b")
    ui.press(Button.SELECT)
    shot(ui, "40_timeline_c_pin")
    ui.press(Button.DOWN)
    shot(ui, "40_timeline_d_pin")
    ui.go_home()
    ui.launch_app("daae3686-bff6-4ba5-921b-262f847bb6e8")
    shot(ui, "40_timeline_e_past")
    ui.press(Button.SELECT)
    shot(ui, "40_timeline_f_past_pin")


def test_health(dut, ui):
    open_settings(ui)
    ui.press(Button.DOWN, presses=8, gap_ms=350)
    ui.wait_idle()
    ui.press(Button.SELECT)
    ui.wait_idle()
    ui.press(Button.SELECT)  # tracking on
    ui.wait_idle()
    for name, idx in (("50_health", 7), ("51_workout", 6)):
        open_app(ui, idx)
        shot(ui, f"{name}_a")
        for i in range(4):
            ui.press(Button.DOWN)
            shot(ui, f"{name}_b{i}")
        ui.press(Button.SELECT)
        shot(ui, f"{name}_c")
        ui.press(Button.DOWN)
        shot(ui, f"{name}_d")


def _pstr(text):
    b = text.encode()
    return struct.pack("<H", len(b)) + b


def insert_weather(dut, name="Austin", phrase="Partly cloudy", current=True):
    types = [0, 7, 3, 1, 7, 4, 0]
    rec = struct.pack(
        "<BhBhhBhhiB", 4, 72, 0, 81, 63, 7, 84, 65, NOW, 1 if current else 0
    )
    rec += struct.pack("<BhhhHHhh", 0, 74, 60, 20, 8, 180, 3027, -9774)
    rec += struct.pack("<B", 7) + b"".join(
        struct.pack("<hhB", 80 + i, 60 + i, t) for i, t in enumerate(types)
    )
    rec += struct.pack("<B", 24) + bytes(
        [0, 0, 7, 7, 7, 7, 7, 7, 0, 0, 1, 1, 3, 3, 3, 0, 0, 7, 7, 7, 7, 7, 7, 0]
    )
    rec += bytes((60 + (h if h < 15 else 30 - h)) & 0xFF for h in range(24))
    strings = _pstr(name) + _pstr(phrase)
    rec += struct.pack("<H", len(strings)) + strings
    key = _uuid.uuid4()
    _blobdb(dut.protocol, "insert", 0x05, key, rec)
    return key


def test_weather(dut, ui):
    keys = [
        insert_weather(dut),
        insert_weather(dut, "Chicago", "Light rain", current=False),
    ]
    prefs = struct.pack("<B", len(keys)) + b"".join(k.bytes for k in keys)
    _blobdb(dut.protocol, "insert", 0x09, _Key(b"weatherApp"), prefs)
    time.sleep(1)
    open_app(ui, 5)
    shot(ui, "60_weather_a")
    for i in range(4):
        ui.press(Button.DOWN)
        shot(ui, f"60_weather_b{i}")
    ui.press(Button.SELECT)
    shot(ui, "60_weather_c")
    for i in range(3):
        ui.press(Button.DOWN)
        shot(ui, f"60_weather_d{i}")
    ui.press(Button.SELECT)
    shot(ui, "60_weather_e")
    ui.press(Button.BACK)
    ui.press(Button.BACK)
    ui.press(Button.UP)
    shot(ui, "60_weather_f")
    ui.press(Button.SELECT)
    shot(ui, "60_weather_g")


def test_weather_frames(dut, ui):
    keys = [insert_weather(dut)]
    prefs = struct.pack("<B", len(keys)) + b"".join(k.bytes for k in keys)
    _blobdb(dut.protocol, "insert", 0x09, _Key(b"weatherApp"), prefs)
    time.sleep(1)
    open_app(ui, 5)
    time.sleep(4)
    for i in range(12):
        ui.screenshot().save(os.path.join(OUT, f"70_frame_{i:02d}.png"))
        time.sleep(0.15)


def test_popups(dut, ui):
    from libpebble2.protocol.phone import (
        CallEnd,
        CallStart,
        IncomingCall,
        PhoneNotification,
    )

    ui.go_home()
    dut.protocol.send_packet(
        PhoneNotification(
            command_id=0x04,
            cookie=7,
            message=IncomingCall(number="+15125550100", name="Anna Lee"),
        )
    )
    time.sleep(2)
    shot(ui, "80_call_incoming")
    dut.protocol.send_packet(
        PhoneNotification(command_id=0x08, cookie=7, message=CallStart())
    )
    time.sleep(2)
    shot(ui, "81_call_started")
    dut.protocol.send_packet(
        PhoneNotification(command_id=0x09, cookie=7, message=CallEnd())
    )
    time.sleep(3)
    ui.go_home()
    # Quick View: an event starting in 5 minutes
    insert_pin(dut, NOW + 300, "Design review", "Room 4B")
    ui.set_time(NOW + 1)
    time.sleep(3)
    shot(ui, "82_quick_view")


DEEP = [
    ("sysinfo", 11, [Button.SELECT], [Button.DOWN, Button.DOWN]),
    ("cert", 11, [Button.DOWN, Button.SELECT], [Button.DOWN]),
    ("bt", 0, [], [Button.DOWN]),
    ("backlight", 7, [Button.SELECT], [Button.DOWN]),
    ("timezone", 6, [Button.DOWN, Button.DOWN, Button.SELECT], [Button.DOWN]),
    ("quiet_sched", 3, [Button.DOWN, Button.DOWN, Button.SELECT], [Button.DOWN]),
    ("notif_filter", 1, [Button.SELECT], [Button.DOWN]),
    ("quicklaunch_app", 5, [Button.SELECT], [Button.DOWN]),
    ("timeline_set", 4, [], [Button.DOWN, Button.SELECT]),
]


@pytest.mark.parametrize("case", DEEP, ids=[c[0] for c in DEEP])
def test_deep_settings(dut, ui, case):
    name, row, inner, after = case
    open_settings(ui)
    if row:
        ui.press(Button.DOWN, presses=row, gap_ms=350)
        ui.wait_idle()
    ui.press(Button.SELECT)
    ui.wait_idle()
    for b in inner:
        ui.press(b)
        ui.wait_idle()
    shot(ui, f"90_{name}_a")
    for i, b in enumerate(after):
        ui.press(b)
        shot(ui, f"90_{name}_b{i}")


def _burst(ui, prefix, n=20, white_limit=None):
    for i in range(n):
        image = ui.screenshot()
        image.save(os.path.join(OUT, f"{prefix}_{i:02d}.png"))
        if white_limit is not None:
            white = image.convert("L").histogram()[255] / (image.width * image.height)
            assert white < white_limit, (
                f"Bright flash in {prefix}, frame {i}: {white:.1%} white"
            )
        time.sleep(0.04)


def test_weather_globe(dut, ui):
    keys = [
        insert_weather(dut),
        insert_weather(dut, "Chicago", "Light rain", current=False),
    ]
    prefs = struct.pack("<B", len(keys)) + b"".join(k.bytes for k in keys)
    _blobdb(dut.protocol, "insert", 0x09, _Key(b"weatherApp"), prefs)
    open_app(ui, 5)
    ui.press(Button.UP, settle=False)
    _burst(ui, "61_weather_card_in", 30)
    time.sleep(1)
    shot(ui, "61_weather_card")
    ui.press(Button.SELECT, settle=False)
    _burst(ui, "62_weather_globe_in", 30)
    time.sleep(1)
    shot(ui, "62_weather_globe_intro")
    ui.press(Button.SELECT, settle=False)
    _burst(ui, "63_weather_globe_reveal", 50, white_limit=0.25 if DARK else None)
    time.sleep(1)
    shot(ui, "63_weather_globe_space")
    ui.press(Button.BACK, settle=False)
    _burst(ui, "64_weather_globe_reverse", 50, white_limit=0.25 if DARK else None)
    shot(ui, "64_weather_globe_return")


def test_transitions(dut, ui):
    insert_pin(dut, NOW + 1800, "Design review", "Room 4B")
    ui.go_home()
    ui.wait_idle()
    ui.press(Button.SELECT, settle=False)
    _burst(ui, "95_launcher_open")
    ui.wait_idle()
    ui.press(Button.UP, presses=12, gap_ms=350)
    ui.wait_idle()
    ui.press(Button.DOWN, presses=2, gap_ms=350)  # Notifications
    ui.wait_idle()
    ui.press(Button.SELECT, settle=False)
    _burst(ui, "96_app_open")
    ui.wait_idle()
    ui.press(Button.BACK, settle=False)
    _burst(ui, "97_app_close")
    ui.go_home()
    ui.wait_idle()
    ui.press(Button.DOWN, settle=False)
    _burst(ui, "98_timeline_in", 30)
    ui.wait_idle()
    ui.press(Button.SELECT, settle=False)
    _burst(ui, "99_pin_open")
