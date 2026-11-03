#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 mFat
"""Widget tests that need no RDP server (run by `meson test`).

Needs a display; exits 77 (meson: skipped) without one. Run headless with
xvfb-run, or GDK_BACKEND=x11 under any X server.
"""
import gc
import sys
import time

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("GtkFrdp", "4.0")
from gi.repository import GLib, Gtk, GtkFrdp  # noqa: E402

SKIP = 77
MAX_UI_STALL = 0.25        # seconds the main loop may be blocked
UNROUTABLE = "10.255.255.1"  # TCP connect hangs until timeout (or fails fast in some sandboxes)

failures = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        failures.append(what)


class StallMeter:
    """Longest gap between 10 ms main-loop ticks."""

    def __init__(self):
        self.last = time.monotonic()
        self.max = 0.0
        self.id = GLib.timeout_add(10, self.tick)

    def tick(self):
        now = time.monotonic()
        self.max = max(self.max, now - self.last)
        self.last = now
        return True

    def stop(self):
        GLib.source_remove(self.id)


def run_until(predicate, timeout):
    ctx = GLib.MainContext.default()
    end = time.monotonic() + timeout
    while not predicate() and time.monotonic() < end:
        ctx.iteration(False)
        time.sleep(0.002)
    return predicate()


def new_window_with_display():
    window = Gtk.Window()
    display = GtkFrdp.Display()
    window.set_child(display)
    window.present()
    return window, display


def test_create_destroy():
    finalized = []
    window, display = new_window_with_display()
    display.weak_ref(lambda *a: finalized.append(True))
    check(not display.is_open(), "new widget is not open")
    window.set_child(None)
    window.destroy()
    # A destroyed GtkWindow still references its last focus widget while it
    # is alive (plain GTK widgets behave the same), so drop it as well.
    del window, display
    gc.collect()
    run_until(lambda: finalized, 2)
    check(bool(finalized), "unconnected widget is freed")


def test_password_write_only():
    display = GtkFrdp.Display()
    display.props.password = "secret"
    try:
        value = display.get_property("password")
        readable = True
    except TypeError:
        readable, value = False, None
    check(not readable and value is None, "password property is write-only")


def test_refused_connection():
    window, display = new_window_with_display()
    state = {"disconnected": False, "failure": None}
    display.connect("rdp-auth-failure", lambda d, m: state.update(failure=m))
    display.connect("rdp-error", lambda d, m: state.update(failure=m))
    display.connect("rdp-disconnected", lambda d: state.update(disconnected=True))

    meter = StallMeter()
    t0 = time.monotonic()
    display.open_host("127.0.0.1", 1)        # nothing listens on port 1
    returned_after = time.monotonic() - t0
    done = run_until(lambda: state["disconnected"], 20)
    meter.stop()

    check(returned_after < 0.1, f"open_host() returns immediately ({returned_after * 1000:.0f} ms)")
    if not done and sys.platform == "darwin":
        # FreeRDP's failed-connect warning resolves the peer with getnameinfo()
        # without NI_NUMERICHOST; on macOS CI runners that reverse lookup of
        # 127.0.0.1 goes to mDNS and stalls longer than this test waits.
        # The session thread is stuck in FreeRDP, the UI is not.
        print("skip refused-connection outcome: FreeRDP reverse lookup stalls on this host")
    else:
        check(done, "refused connection ends in rdp-disconnected")
        check(state["failure"] is not None, f"failure is reported ({state['failure']!r})")
    check(meter.max < MAX_UI_STALL, f"UI not blocked while failing (max stall {meter.max * 1000:.0f} ms)")

    finalized = []
    display.weak_ref(lambda *a: finalized.append(True))
    window.set_child(None)
    window.destroy()
    del window, display
    gc.collect()
    run_until(lambda: finalized, 2)
    check(bool(finalized), "widget is freed after a failed connection")


def test_close_while_connecting():
    window, display = new_window_with_display()
    state = {"disconnected_at": None, "failure": None}
    display.connect("rdp-disconnected", lambda d: state.update(disconnected_at=time.monotonic()))
    display.connect("rdp-auth-failure", lambda d, m: state.update(failure=m))

    meter = StallMeter()
    display.open_host(UNROUTABLE, 3389)
    run_until(lambda: False, 0.5)
    if state["disconnected_at"] is not None:
        print("skip close-while-connecting: unroutable address failed immediately here")
        meter.stop()
        window.destroy()
        return
    t_close = time.monotonic()
    display.close()
    done = run_until(lambda: state["disconnected_at"] is not None, 5)
    meter.stop()

    check(done, "close() during a pending connect ends the session")
    if done:
        took = state["disconnected_at"] - t_close
        check(took < 1.0, f"close() while connecting takes effect quickly ({took * 1000:.0f} ms)")
    check(state["failure"] is None, "a cancelled connect is not reported as a failure")
    check(meter.max < MAX_UI_STALL, f"UI not blocked (max stall {meter.max * 1000:.0f} ms)")
    window.destroy()


def main():
    if not Gtk.init_check():
        print("no display available; skipping")
        return SKIP
    for test in (test_create_destroy, test_password_write_only,
                 test_refused_connection, test_close_while_connecting):
        print(f"# {test.__name__}")
        test()
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
