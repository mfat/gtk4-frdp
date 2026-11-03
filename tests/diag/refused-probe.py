# SPDX-License-Identifier: GPL-3.0-or-later
"""Diagnostic: connect to a closed local port and report what happens.

Used by CI on macOS when the widget tests fail: run it in the background,
sample its threads while it waits, and read FreeRDP's debug log.
"""
import os
import sys
import time

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("GtkFrdp", "4.0")
from gi.repository import GLib, Gtk, GtkFrdp  # noqa: E402

print("pid", os.getpid(), flush=True)
window = Gtk.Window()
display = GtkFrdp.Display()
window.set_child(display)
window.present()
start = time.monotonic()
for name in ("rdp-disconnected", "rdp-connected"):
    display.connect(name, lambda d, n=name: print(f"{time.monotonic() - start:.2f}s {n}", flush=True))
for name in ("rdp-error", "rdp-auth-failure"):
    display.connect(name, lambda d, m, n=name: print(f"{time.monotonic() - start:.2f}s {n}: {m}", flush=True))
display.open_host("127.0.0.1", 1)
print(f"open_host returned after {time.monotonic() - start:.2f}s", flush=True)
# A second widget: is the first call's cost one-time initialisation?
second = GtkFrdp.Display()
t1 = time.monotonic()
second.open_host("127.0.0.1", 1)
print(f"second open_host returned after {time.monotonic() - t1:.2f}s", flush=True)

loop = GLib.MainLoop()
GLib.timeout_add_seconds(int(sys.argv[1]) if len(sys.argv) > 1 else 15, loop.quit)
loop.run()
print(f"done after {time.monotonic() - start:.2f}s", flush=True)
