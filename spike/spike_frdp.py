#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 mFat
"""gtk-frdp GTK4 port spike: same checks as the RDW spike.

Usage: spike_frdp.py HOST PORT USER PASSWORD_FILE [--cycles N] [--hold S] [--idle-cpu S]

Logs per line: elapsed, current RSS, thread count, and the longest main-loop
stall seen so far (a 10 ms ticker measures how long the UI was frozen).
"""
import argparse
import gc
import os
import sys
import time

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("GtkFrdp", "4.0")
from gi.repository import GLib, Gtk, GtkFrdp  # noqa: E402

T0 = time.monotonic()
STALL = {"last": time.monotonic(), "max": 0.0}


def rss_mb():
    with open("/proc/self/status") as fh:
        for line in fh:
            if line.startswith("VmRSS"):
                return int(line.split()[1]) // 1024
    return -1


def log(msg):
    print(f"[{time.monotonic() - T0:7.2f}s rss={rss_mb()}MB "
          f"thr={len(os.listdir('/proc/self/task'))} "
          f"maxstall={STALL['max'] * 1000:.0f}ms] {msg}", flush=True)


def ticker():
    now = time.monotonic()
    STALL["max"] = max(STALL["max"], now - STALL["last"])
    STALL["last"] = now
    return True


def cpu_seconds():
    t = os.times()
    return t.user + t.system


class Spike(Gtk.Application):
    def __init__(self, args):
        super().__init__(application_id="io.github.mfat.sshpilot.FrdpSpike")
        self.args = args
        with open(args.password_file) as fh:
            self.password = fh.read().strip()
        self.cycle = 0
        self.display = None

    def do_activate(self):
        self.win = Gtk.Window(application=self, title="gtk-frdp GTK4 spike")
        self.win.set_default_size(1280, 800)
        self.win.present()
        GLib.timeout_add(10, ticker)
        GLib.timeout_add(300, lambda: (self.start_cycle(), False)[1])

    def start_cycle(self):
        self.cycle += 1
        display = GtkFrdp.Display()
        display.set_property("username", self.args.user)
        display.set_property("password", self.password)
        display.set_property("allow-resize", True)
        display.connect("rdp-connected", self.on_connected)
        display.connect("rdp-disconnected", self.on_disconnected)
        display.connect("rdp-error", lambda d, m: log(f"rdp-error: {m}"))
        display.connect("rdp-auth-failure", lambda d, m: log(f"rdp-auth-failure: {m}"))
        display.connect("rdp-needs-certificate-verification", self.on_cert)
        n = self.cycle
        display.weak_ref(lambda *a: log(f"widget of cycle {n} FINALIZED"))
        self.win.set_child(display)
        self.display = display
        STALL["max"] = 0.0
        STALL["last"] = time.monotonic()
        self.t_connect = time.monotonic()
        log(f"cycle {self.cycle}: connecting to {self.args.host}:{self.args.port}")
        # Synchronous in gtk-frdp: returns after freerdp_connect() finished.
        display.open_host(self.args.host, self.args.port)
        log(f"cycle {self.cycle}: open_host returned after "
            f"{time.monotonic() - self.t_connect:.2f}s")
        if self.args.abort_after:
            GLib.timeout_add(self.args.abort_after, self.abort_connect)
        return False

    def abort_connect(self):
        log(f"cycle {self.cycle}: close() while connecting")
        self.t_abort = time.monotonic()
        self.display.close()
        return False

    def on_cert(self, display, host, port, cn, subject, issuer, fingerprint, flags):
        log(f"certificate: cn={cn} fp={fingerprint[:23]}... -> accept for session (spike)")
        display.certificate_verify_ex_finish(2)

    def on_connected(self, display):
        log(f"cycle {self.cycle}: rdp-connected")
        if self.args.idle_cpu:
            GLib.timeout_add_seconds(5, self.measure_idle_cpu)
        elif self.args.hold:
            GLib.timeout_add_seconds(self.args.hold, self.end_cycle)

    def measure_idle_cpu(self):
        self.cpu0, self.wall0 = cpu_seconds(), time.monotonic()
        GLib.timeout_add_seconds(self.args.idle_cpu, self.report_idle_cpu)
        return False

    def report_idle_cpu(self):
        cpu = cpu_seconds() - self.cpu0
        wall = time.monotonic() - self.wall0
        log(f"idle CPU over {wall:.1f}s: {100 * cpu / wall:.1f}% of one core")
        if self.args.hold:
            GLib.timeout_add_seconds(1, self.end_cycle)
        return False

    def end_cycle(self):
        log(f"cycle {self.cycle}: closing")
        self.display.close()
        return False

    def on_disconnected(self, display):
        extra = ""
        if getattr(self, "t_abort", None):
            extra = f" ({time.monotonic() - self.t_abort:.2f}s after close)"
        log(f"cycle {self.cycle}: rdp-disconnected{extra}")
        if display is not self.display:
            return
        self.win.set_child(None)
        self.display = None
        gc.collect()
        if self.cycle < self.args.cycles:
            GLib.timeout_add(500, lambda: (self.start_cycle(), False)[1])
        elif self.args.cycles:
            GLib.timeout_add(1000, self.finish)

    def finish(self):
        gc.collect()
        log("all cycles done")
        self.quit()
        return False


def main():
    p = argparse.ArgumentParser()
    p.add_argument("host")
    p.add_argument("port", type=int)
    p.add_argument("user")
    p.add_argument("password_file")
    p.add_argument("--cycles", type=int, default=0)
    p.add_argument("--hold", type=int, default=0)
    p.add_argument("--idle-cpu", type=int, default=0)
    p.add_argument("--abort-after", type=int, default=0, help="ms after open_host to call close()")
    args = p.parse_args()
    log(f"pid {os.getpid()}")
    return Spike(args).run([sys.argv[0]])


if __name__ == "__main__":
    sys.exit(main())
