#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 mFat
"""Live check: the remote cursor is scaled together with the desktop.

Same server settings as live-session.py (GTK4_FRDP_TEST_HOST, ...); also
needs xdotool and an X11 display to move the pointer over the desktop (the
server only sends its cursor image once the pointer moves). Skipped (77)
otherwise. Assumes FreeRDP's default 1024x768 desktop (allow-resize off).
"""
import math
import os
import shutil
import subprocess
import sys
import time

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("GtkFrdp", "4.0")
from gi.repository import GLib, Gtk, GtkFrdp  # noqa: E402

env = os.environ.get


def spin(seconds):
    ctx = GLib.MainContext.default()
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        ctx.iteration(False)
        time.sleep(0.005)


def cursor_info(display):
    cursor = display.get_cursor()
    texture = cursor.get_texture() if cursor else None
    if texture is None:
        return None
    return texture.get_width(), texture.get_height(), cursor.get_hotspot_x(), cursor.get_hotspot_y()


def main():
    host = env("GTK4_FRDP_TEST_HOST")
    if not host or not shutil.which("xdotool") or not env("DISPLAY"):
        print("needs GTK4_FRDP_TEST_HOST, xdotool and an X11 DISPLAY; skipping")
        return 77
    if not Gtk.init_check():
        return 77
    password = env("GTK4_FRDP_TEST_PASSWORD")
    if password is None and env("GTK4_FRDP_TEST_PASSWORD_FILE"):
        with open(env("GTK4_FRDP_TEST_PASSWORD_FILE")) as fh:
            password = fh.read().strip()

    window = Gtk.Window(title="gtk4-frdp-cursor-test")
    window.set_default_size(512, 384)           # half of 1024x768
    display = GtkFrdp.Display(scaling=True)
    if env("GTK4_FRDP_TEST_USER"):
        display.props.username = env("GTK4_FRDP_TEST_USER")
    if password:
        display.props.password = password
    display.connect("rdp-needs-certificate-verification",
                    lambda d, *a: d.certificate_verify_ex_finish(2))
    state = {"connected": False}
    display.connect("rdp-connected", lambda d: state.update(connected=True))
    window.set_child(display)
    window.present()
    display.open_host(host, int(env("GTK4_FRDP_TEST_PORT", "3389")))
    spin(5)
    if not state["connected"]:
        print("FAIL could not connect")
        return 1

    xid = subprocess.run(["xdotool", "search", "--name", "gtk4-frdp-cursor-test"],
                         capture_output=True, text=True).stdout.split()[0]
    for x, y in ((100, 100), (200, 150), (250, 200)):
        subprocess.run(["xdotool", "mousemove", "--window", xid, str(x), str(y)])
        spin(0.5)
    spin(2)

    scale = min(display.get_width() / 1024, display.get_height() / 768)
    scaled = cursor_info(display)
    display.props.scaling = False
    spin(1.5)
    full = cursor_info(display)
    display.close()
    spin(1)

    print(f"scale {scale:.2f}: scaled cursor {scaled}, unscaled {full}")
    if scaled is None or full is None:
        print("FAIL server sent no cursor image")
        return 1
    ok = (scaled[0] == max(1, math.ceil(full[0] * scale)) and
          scaled[1] == max(1, math.ceil(full[1] * scale)) and
          scaled[2] == math.floor(full[2] * scale) and
          scaled[3] == math.floor(full[3] * scale))
    print("ok   cursor scaled with the desktop" if ok else "FAIL cursor not scaled as expected")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
