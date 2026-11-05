#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 mFat
"""Widget tests that need no RDP server (run by `meson test`).

Needs a display; exits 77 (meson: skipped) without one. Run headless with
xvfb-run, or GDK_BACKEND=x11 under any X server.
"""
import gc
import socket
import sys
import threading
import time

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("GtkFrdp", "4.0")
from gi.repository import GLib, Gtk, GtkFrdp  # noqa: E402

SKIP = 77
MAX_UI_STALL = 0.25        # seconds the main loop may be blocked
UNROUTABLE = "10.255.255.1"  # TCP connect hangs until timeout (or fails fast in some sandboxes)
# FreeRDP's FREERDP_ERROR_CONNECT_FAILED and ..._CONNECT_TRANSPORT_FAILED.
CONNECT_FAILED = 0x00020006
CONNECT_TRANSPORT_FAILED = 0x0002000D

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


class ClosingServer:
    """Loopback TCP server that accepts and immediately closes.

    FreeRDP then fails during its RDP handshake, a deterministic failure
    that does not go through the refused-connect path (whose warning does a
    reverse DNS lookup that stalls on some macOS hosts, see
    test_refused_port)."""

    def __init__(self):
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(4)
        self.port = self.sock.getsockname()[1]
        threading.Thread(target=self._serve, daemon=True).start()

    def _serve(self):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            conn.close()

    def close(self):
        self.sock.close()


def watch(display):
    """Record the failure: its sentence, the signal it came on, its code."""
    state = {"disconnected": 0, "failure": None, "signal": None, "code": None}

    def on_failure(widget, message, signal):
        state.update(failure=message, signal=signal,
                     code=widget.props.last_error_code)

    display.connect("rdp-auth-failure", lambda d, m: on_failure(d, m, "rdp-auth-failure"))
    display.connect("rdp-error", lambda d, m: on_failure(d, m, "rdp-error"))
    display.connect("rdp-disconnected",
                    lambda d: state.update(disconnected=state["disconnected"] + 1))
    return state


def check_freed(refs, what):
    """refs = [window, display]; the caller must hold no other reference."""
    window, display = refs
    refs.clear()
    finalized = []
    display.weak_ref(lambda *a: finalized.append(True))
    window.set_child(None)
    window.destroy()
    del window, display
    gc.collect()
    run_until(lambda: finalized, 2)
    check(bool(finalized), what)


def test_cold_start():
    """The process's first connection: FreeRDP is initialised for the first
    time, which must happen on the session thread, not the main loop."""
    server = ClosingServer()
    window, display = new_window_with_display()
    state = watch(display)

    meter = StallMeter()
    t0 = time.monotonic()
    display.open_host("127.0.0.1", server.port)
    returned_after = time.monotonic() - t0
    done = run_until(lambda: state["disconnected"] > 0, 20)
    meter.stop()

    check(returned_after < 0.1, f"first open_host() returns immediately ({returned_after * 1000:.0f} ms)")
    check(meter.max < MAX_UI_STALL, f"UI not blocked on a cold start (max stall {meter.max * 1000:.0f} ms)")
    check(done, "a dropped connection ends in rdp-disconnected")
    window.destroy()
    server.close()


def test_handshake_failure():
    server = ClosingServer()
    window, display = new_window_with_display()
    state = watch(display)

    display.open_host("127.0.0.1", server.port)
    done = run_until(lambda: state["disconnected"] > 0, 20)
    run_until(lambda: False, 0.3)          # a second emission would arrive now
    server.close()

    check(done, "handshake failure ends in rdp-disconnected")
    check(state["failure"] is not None, f"handshake failure is reported ({state['failure']!r})")
    # The server dropped the connection during the handshake: that is a
    # connection failure, not a refused logon.
    check(state["signal"] == "rdp-error",
          f"a dropped handshake is a connection failure ({state['signal']})")
    # FreeRDP retries a failed connect once, and the retry clears the error it
    # recorded; gtk4-frdp restores the first error (the transport one) when
    # FreeRDP has the setting that records the retry (3.26+). Older builds
    # keep the generic CONNECT_FAILED.
    check(state["code"] in (CONNECT_TRANSPORT_FAILED, CONNECT_FAILED),
          f"the failure carries FreeRDP's code ({state['code']})")
    if state["code"] == CONNECT_TRANSPORT_FAILED:
        check("transport" in state["failure"].lower(),
              f"the code and the sentence agree ({state['failure']!r})")
    check(state["disconnected"] == 1, f"rdp-disconnected is emitted once ({state['disconnected']})")
    check(not display.is_open(), "widget is not open after the failure")
    refs = [window, display]
    del window, display
    check_freed(refs, "widget is freed after a failed connection")


def test_refused_port():
    window, display = new_window_with_display()
    state = watch(display)

    display.open_host("127.0.0.1", 1)        # nothing listens on port 1
    done = run_until(lambda: state["disconnected"] > 0, 20)
    run_until(lambda: False, 0.3)

    if not done and sys.platform == "darwin":
        # Known FreeRDP limitation, tracked upstream: its failed-connect
        # warning resolves the peer with getnameinfo() without
        # NI_NUMERICHOST, and on macOS CI runners that reverse lookup of
        # 127.0.0.1 stalls in mDNS for longer than this test waits. Only
        # the session thread waits; the UI does not (test_cold_start).
        print("skip refused-port outcome: FreeRDP's reverse lookup stalls on this host")
        window.destroy()
        return
    check(done, "refused connection ends in rdp-disconnected")
    check(state["failure"] is not None, f"refused connection is reported ({state['failure']!r})")
    check(state["signal"] == "rdp-error",
          f"a refused connect is a connection failure ({state['signal']})")
    check(state["code"] == CONNECT_FAILED,
          f"a refused connect is CONNECT_FAILED ({state['code']})")
    check(state["disconnected"] == 1, f"rdp-disconnected is emitted once ({state['disconnected']})")
    refs = [window, display]
    del window, display
    check_freed(refs, "widget is freed after a refused connection")


def test_close_while_initializing():
    """close() in the same main-loop turn as open_host(): the session thread
    is still creating the FreeRDP instance."""
    window, display = new_window_with_display()
    state = watch(display)

    display.open_host(UNROUTABLE, 3389)
    t_close = time.monotonic()
    display.close()
    done = run_until(lambda: state["disconnected"] > 0, 5)
    took = time.monotonic() - t_close
    run_until(lambda: False, 0.3)

    check(done, "close() during setup ends the session")
    check(took < 1.0, f"close() during setup takes effect quickly ({took * 1000:.0f} ms)")
    check(state["failure"] is None, "a cancelled setup is not reported as a failure")
    check(state["disconnected"] == 1, f"rdp-disconnected is emitted once ({state['disconnected']})")
    refs = [window, display]
    del window, display
    check_freed(refs, "widget is freed after closing during setup")


def test_reconnect_after_failure():
    server = ClosingServer()
    window, display = new_window_with_display()
    state = watch(display)
    for attempt in (1, 2):
        before = state["disconnected"]
        display.open_host("127.0.0.1", server.port)
        done = run_until(lambda: state["disconnected"] > before, 20)
        check(done, f"attempt {attempt} on the same widget completes")
    server.close()
    window.destroy()


def test_close_while_connecting():
    window, display = new_window_with_display()
    state = {"disconnected_at": None, "failure": None}
    display.connect("rdp-disconnected", lambda d: state.update(disconnected_at=time.monotonic()))
    display.connect("rdp-auth-failure", lambda d, m: state.update(failure=m))

    meter = StallMeter()
    t0 = time.monotonic()
    display.open_host(UNROUTABLE, 3389)
    check(time.monotonic() - t0 < 0.1,
          f"open_host() returns immediately ({(time.monotonic() - t0) * 1000:.0f} ms)")
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
    # test_cold_start must make the process's first connection.
    for test in (test_create_destroy, test_password_write_only, test_cold_start,
                 test_handshake_failure, test_refused_port,
                 test_close_while_initializing, test_close_while_connecting,
                 test_reconnect_after_failure):
        print(f"# {test.__name__}")
        test()
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
