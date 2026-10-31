# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

"""Dark-mode repainting, round-trip rendering, and preference persistence."""

from pathlib import Path

import pytest
from harness.helpers.ui import Button
from PIL import ImageChops

pytestmark = [
    pytest.mark.requires_config("CONFIG_THEMING"),
    pytest.mark.platforms("gabbro"),
]

SETTINGS = "07e0d9cb-8957-4bf7-9d42-35bf47caadfe"
NOW = 1767261600


def _themes(ui):
    ui.go_home()
    ui.launch_app(SETTINGS)
    ui.wait_idle()
    ui.press(Button.DOWN, presses=9, gap_ms=350)
    ui.wait_idle()
    ui.press(Button.SELECT)
    return ui.wait_idle()


def _is_dark(image):
    # Empty space below both theme rows, inside the round display.
    color = image.getpixel((130, 215))
    assert color in ((0, 0, 0), (255, 255, 255))
    return color == (0, 0, 0)


def test_theme_toggle_repaints_and_restores(dut, ui, test_results_dir):
    ui.set_time(NOW)
    light = _themes(ui)
    if _is_dark(light):
        ui.press(Button.SELECT)
        light = ui.wait_idle()
    light.save(Path(test_results_dir) / "themes-light.png")
    for _ in range(3):
        ui.press(Button.SELECT)
        dark = ui.wait_idle()
        assert _is_dark(dark)
        ui.press(Button.SELECT)
        restored = ui.wait_idle()
        assert not _is_dark(restored)
        assert ImageChops.difference(light, restored).getbbox() is None
    dark.save(Path(test_results_dir) / "themes-dark.png")


def test_dark_mode_survives_restart(dut, ui, test_results_dir):
    ui.set_time(NOW)
    if not _is_dark(_themes(ui)):
        ui.press(Button.SELECT)
        assert _is_dark(ui.wait_idle())
    dut.reset()
    assert _is_dark(_themes(ui))
    ui.press(Button.SELECT)
    assert not _is_dark(ui.wait_idle())
    dut.reset()
    restored = _themes(ui)
    assert not _is_dark(restored)
    restored.save(Path(test_results_dir) / "themes-after-restart.png")
