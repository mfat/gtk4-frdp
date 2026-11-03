#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 mFat
"""Live check: text and image clipboard in both directions (Windows server).

Same server settings as live-session.py; also needs xdotool and an X11
DISPLAY (keystrokes in the remote Notepad and Paint). Skipped (77)
otherwise. Uses Win+R -> notepad / mspaint, so the server must be Windows.

Every check is built so that it cannot pass on stale local data: text that
went to the server comes back with a suffix typed remotely, and data read
back must come from the widget's own provider (the server), not from what
the test put on the clipboard.
"""
import os
import shutil
import subprocess
import sys
import time

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("GtkFrdp", "4.0")
from gi.repository import Gdk, GLib, Gtk, GtkFrdp  # noqa: E402

W, H = 120, 80              # test image: left half red, right half blue

env = os.environ.get
TO_REMOTE = "Local→remote ü ☺ سلام 123"
TO_LOCAL = "Remote to local 456"
failures = []


def spin(seconds):
    ctx = GLib.MainContext.default()
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        ctx.iteration(False)
        time.sleep(0.005)


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond:
        failures.append(what)


def read_local_text(clipboard, timeout=10):
    result = {}

    def done(cb, res):
        try:
            result["text"] = cb.read_text_finish(res)
        except GLib.Error as e:
            result["error"] = e.message

    clipboard.read_text_async(None, done)
    end = time.monotonic() + timeout
    while not result and time.monotonic() < end:
        spin(0.05)
    return result.get("text"), result.get("error")


def read_local_texture(clipboard, timeout=10):
    result = {}

    def done(cb, res):
        try:
            result["texture"] = cb.read_texture_finish(res)
        except GLib.Error as e:
            result["error"] = e.message

    clipboard.read_texture_async(None, done)
    end = time.monotonic() + timeout
    while not result and time.monotonic() < end:
        spin(0.05)
    return result.get("texture"), result.get("error")


def pixel(texture, x, y):
    downloader = Gdk.TextureDownloader.new(texture)
    downloader.set_format(Gdk.MemoryFormat.R8G8B8A8)
    data, stride = downloader.download_bytes()
    raw = data.get_data()
    return tuple(raw[y * stride + x * 4: y * stride + x * 4 + 3])


def test_image():
    row = b"".join(bytes([255, 0, 0, 255]) if x < W // 2 else bytes([0, 0, 255, 255])
                   for x in range(W))
    return Gdk.MemoryTexture.new(W, H, Gdk.MemoryFormat.R8G8B8A8,
                                 GLib.Bytes.new(row * H), W * 4)


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

    window = Gtk.Window(title="gtk4-frdp-clipboard-test")
    window.set_default_size(1024, 768)
    display = GtkFrdp.Display()
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
    spin(20)                      # a fresh logon takes a while
    if not state["connected"]:
        print("FAIL could not connect")
        return 1

    xid = subprocess.run(["xdotool", "search", "--name", "gtk4-frdp-clipboard-test"],
                         capture_output=True, text=True).stdout.split()[0]

    def keys(*k):
        subprocess.run(["xdotool", "key", "--window", xid, *k])
        spin(0.5)

    def type_text(text):
        subprocess.run(["xdotool", "type", "--window", xid, "--delay", "120", text])
        spin(1)

    subprocess.run(["xdotool", "mousemove", "--window", xid, "500", "380", "click", "1"])
    spin(1)
    keys("super+r")
    spin(2)
    type_text("notepad")
    keys("Return")
    spin(4)

    clipboard = display.get_clipboard()

    # local -> remote: put text on the local clipboard, paste it remotely,
    # add a suffix there, and copy everything back: only text that really
    # reached the server can come back with the suffix.
    clipboard.set(TO_REMOTE)
    spin(2)                       # the format list reaches the server
    keys("ctrl+v")
    spin(2)
    type_text(" +R")
    keys("ctrl+a")
    keys("ctrl+c")
    spin(3)                       # server announces its formats; we take ownership
    text, error = read_local_text(clipboard)
    check(text == TO_REMOTE + " +R", f"local -> remote text, edited remotely, back ({text!r}, {error})")

    # remote -> local: type in Notepad, copy, read locally.
    keys("ctrl+a")
    type_text(TO_LOCAL)
    keys("ctrl+a")
    keys("ctrl+c")
    spin(3)
    text, error = read_local_text(clipboard)
    check(text == TO_LOCAL, f"remote -> local ({text!r}, {error})")

    content = clipboard.get_content()
    check(content is not None and clipboard.is_local(),
          "the widget owns the local clipboard while it holds remote data")

    # leave Notepad without saving
    keys("alt+F4")
    spin(1)
    keys("n")   # "Don't Save" (English Windows)
    spin(1)

    # images: paste into Paint (local -> remote), copy the pasted selection
    # (remote -> local), read it back.
    keys("super+r")
    spin(2)
    type_text("mspaint")
    keys("Return")
    spin(6)
    image = Gdk.ContentProvider.new_for_value(test_image())
    clipboard.set_content(image)
    spin(2)
    keys("ctrl+v")
    spin(3)
    keys("ctrl+c")
    spin(4)
    check(clipboard.get_content() is not image, "Paint's copy replaced the test image on the clipboard")
    texture, error = read_local_texture(clipboard)
    if texture is None:
        check(False, f"remote image read back ({error})")
    else:
        size = (texture.get_width(), texture.get_height())
        colours = (pixel(texture, 10, H // 2), pixel(texture, W - 10, H // 2))
        check(size == (W, H) and colours == ((255, 0, 0), (0, 0, 255)),
              f"image local -> Paint -> local ({size}, {colours})")
    keys("alt+F4")
    spin(1)
    keys("n")
    display.close()
    spin(2)
    check(clipboard.get_content() is None or not clipboard.is_local(),
          "remote data is withdrawn from the clipboard when the session ends")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
