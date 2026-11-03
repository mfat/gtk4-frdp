#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 mFat
"""Minimal remote-desktop viewer using gtk4-frdp from Python.

Usage: gtk4-frdp-viewer.py HOST[:PORT] [USER]
The password is read from $GTK4_FRDP_PASSWORD, or asked for in a dialog if
the server requests credentials.

When running from a build tree without installing:
  LD_LIBRARY_PATH=_build/src GI_TYPELIB_PATH=_build/src ./gtk4-frdp-viewer.py HOST
"""
import os
import sys

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("GtkFrdp", "4.0")
from gi.repository import Gio, Gtk, GtkFrdp  # noqa: E402

# Values for certificate_verify_ex_finish()
REJECT, ACCEPT_AND_STORE, ACCEPT_ONCE = 0, 1, 2


class Viewer(Gtk.Application):
    def __init__(self, host, port, user):
        super().__init__(application_id="io.github.gtk4_frdp.PyViewer",
                         flags=Gio.ApplicationFlags.NON_UNIQUE)
        self.host, self.port, self.user = host, port, user

    def do_activate(self):
        self.window = Gtk.ApplicationWindow(application=self, title="Connecting…")
        self.window.set_default_size(1280, 800)

        display = GtkFrdp.Display()
        # Ask the server to follow the widget size instead of scaling.
        display.props.allow_resize = True
        if self.user:
            display.props.username = self.user
        if os.environ.get("GTK4_FRDP_PASSWORD"):
            display.props.password = os.environ["GTK4_FRDP_PASSWORD"]

        display.connect("rdp-needs-certificate-verification", self.on_certificate)
        display.connect("rdp-needs-authentication", self.on_authentication)
        display.connect("rdp-error", lambda d, msg: print("Error:", msg, file=sys.stderr))
        display.connect("rdp-auth-failure", lambda d, msg: print("Login failed:", msg, file=sys.stderr))
        display.connect("rdp-connected", self.on_connected)
        display.connect("rdp-disconnected", lambda d: self.window.destroy())

        self.window.set_child(display)
        self.window.present()
        # Returns immediately: connecting happens on the session thread.
        display.open_host(self.host, self.port)

    def on_connected(self, display):
        self.window.set_title(f"{self.host} — gtk4-frdp")
        display.grab_focus()

    def on_certificate(self, display, host, port, common_name, subject, issuer,
                       fingerprint, flags):
        dialog = Gtk.AlertDialog(
            message=f"Unknown certificate for {host}:{port}",
            detail=f"Common name: {common_name}\nIssuer: {issuer}\nFingerprint:\n{fingerprint}",
            buttons=["Cancel", "Accept once", "Accept and remember"],
            cancel_button=0)

        def done(dialog, result):
            try:
                choice = dialog.choose_finish(result)
            except Exception:
                choice = 0
            display.certificate_verify_ex_finish(
                {1: ACCEPT_ONCE, 2: ACCEPT_AND_STORE}.get(choice, REJECT))

        # The session thread waits for the answer; the UI keeps running.
        dialog.choose(self.window, None, done)

    def on_authentication(self, display):
        dialog = Gtk.Window(transient_for=self.window, modal=True, title="Log in")
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6,
                      margin_top=12, margin_bottom=12, margin_start=12, margin_end=12)
        user = Gtk.Entry(placeholder_text="User name", text=self.user or "")
        password = Gtk.PasswordEntry(placeholder_text="Password", show_peek_icon=True)
        button = Gtk.Button(label="Log in")
        for w in (user, password, button):
            box.append(w)
        dialog.set_child(box)

        answered = {"done": False}

        def answer(*_):
            answered["done"] = True
            display.authenticate_finish(user.get_text(), password.get_text(), None)
            dialog.destroy()

        def cancelled(*_):
            if not answered["done"]:
                display.authenticate_finish(None, None, None)  # cancels the login
            return False

        button.connect("clicked", answer)
        password.connect("activate", answer)
        dialog.connect("close-request", cancelled)
        dialog.present()


def main():
    if len(sys.argv) < 2:
        print(__doc__.splitlines()[2], file=sys.stderr)
        return 2
    host, _, port = sys.argv[1].partition(":")
    user = sys.argv[2] if len(sys.argv) > 2 else None
    return Viewer(host, int(port or 3389), user).run([sys.argv[0]])


if __name__ == "__main__":
    sys.exit(main())
