# gtk-frdp → GTK4 port spike — 2026-11-03

Question: can gtk-frdp's display + session components be ported to GTK4 and
drive a FreeRDP 3 desktop from Python on Linux, as the base for a native
cross-platform library (the alternative to RDW, see `../rdw/FINDINGS.md`)?

**Answer: yes on Linux. Parity with RDW on every check, better on leaks and
certificates, one blocker left: the connect still runs on the UI thread.**
macOS/Windows not attempted yet.

Source: clone of gitlab.gnome.org/GNOME/gtk-frdp at `83854a2` (2026-07-24),
port committed in this clone (`git log`). Library `libgtk4-frdp-4.0.so`, GIR
`GtkFrdp-4.0`. Built against FreeRDP 3.32.1 / GTK 4.22.4 (Ubuntu 26.04).

Build: `meson setup /tmp/gtk4-frdp-build --prefix=$PWD/inst --libdir=lib && ninja -C /tmp/gtk4-frdp-build install`
(needs `freerdp3-dev libwinpr3-dev`). Run: `spike/run.sh [--cycles N --hold S | --idle-cpu S]`.

## What the port changed

| Upstream (GTK3) | Port (GTK4) |
|---|---|
| `GtkDrawingArea` + `key_press_event`/`button_press_event`/... vfuncs | `GtkWidget` + `GtkEventControllerKey/Motion/Scroll`, `GtkGestureClick` (all buttons) |
| `draw` signal, cairo surface wrapping FreeRDP's live buffer | `snapshot()` uploads an immutable `GdkMemoryTexture` (BGRX) once per frame, only when FreeRDP painted |
| `configure-event` + assumes a `GtkScrolledWindow` ancestor | `size_allocate()` on the widget itself, remote resize debounced 200 ms |
| `gdk_seat_grab()` keyboard grab | `gdk_toplevel_inhibit_system_shortcuts()` while the pointer is over the desktop |
| cursors via cairo + `gdk_window_set_cursor` | `gdk_cursor_new_from_texture` + `gtk_widget_set_cursor` |
| colour depth from `GdkVisual` | fixed 32-bit BGRX |
| `GdkEventKey->hardware_keycode` (XKB) | keycode per platform: XKB (Linux), Apple keycodes (macOS), Win32 VK (Windows) — only Linux exercised |
| idle callback blocking the UI thread in `WaitForMultipleObjects(20 ms)` (~50 wake-ups/s) | a `GSource` that hands FreeRDP's handles to the GLib main loop (fds via `winpr_Handle_getFd`; HANDLEs on Windows) |
| FUSE3 required (file clipboard) | clipboard behind `-Dclipboard=true` (off) |

## Upstream bugs found and fixed

1. **Widget never freed**: `open_host()` took a reference that was never
   released, and the widget never released its session.
2. **FreeRDP context leaked per session**: `freerdp_free()` frees only the
   instance struct (checked in FreeRDP 3.32.1 source); upstream never called
   `freerdp_context_free()`.
3. **Use-after-free after a failed connect**: the deferred close was queued
   with a bare pointer → `Gtk-CRITICAL`, then `free(): invalid pointer` abort
   (hidden upstream only because nothing was ever freed). Reproduced, fixed.
4. **Certificate prompt answered synchronously hung forever** (waiting flag
   set after the signal).
5. **Hidden widget stopped network processing** (unmapped → the update loop
   returned early), which would stall background tabs.

## Results (same Windows 10 VM and checks as the RDW spike)

| Check | gtk-frdp GTK4 port | RDW |
|---|---|---|
| Connect (LAN) | 0.14–0.25 s | 0.2–0.4 s |
| Render | ✅ `screenshots/01` | ✅ |
| Mouse + keyboard incl. `!@#` | ✅ `02` | ✅ |
| Window resize → real remote resolution change | ✅ `03`, `04` | ✅ |
| Certificate prompt | ✅ host, CN, subject, issuer, fingerprint, flags; accept-once not stored | raw DER bytes → bool |
| Widget freed after disconnect | ✅ 10/10 | ❌ never |
| Memory over 10 cycles | flat (230–350 MB, ends 237 MB) | climbs ~13–20 MB per cycle |
| Idle CPU, settled desktop | ~0.7–0.9 % of a core | ~0 % |
| UI freeze during connect | **whole connect** (0.2 s LAN, **15 s** unreachable host) | none (async) |
| Protocol engine | FreeRDP 3 (H.264, gfx pipeline, Kerberos, gateway available) | IronRDP, gfx pipeline off |

Correction to an earlier claim in the session: the original 20 ms polling loop
cost ~0.7 % of a core idle, not 7 % (a ticks-vs-percent mistake). The GSource
does not lower idle CPU measurably; it removes the 50 Hz wake-ups and the up to
20 ms input latency on the UI thread.

## Remaining work before this could ship

1. **Blocking connect (blocker).** `freerdp_connect()` runs on the GTK thread;
   upstream disabled its threaded variant over "random race conditions". Needs
   a connect worker with all FreeRDP callbacks (certificate, auth, PostConnect,
   GDI setup) marshalled to the main thread, or a full worker-owned session.
2. Prompts still spin a nested main loop (`g_main_context_iteration` until
   answered) — re-entrancy hazard; move to request/finish objects.
3. Clipboard: text-only path without FUSE; file clipboard Linux-only.
4. Keyboard layout detection reads GNOME settings (Linux only).
5. Cursor is not scaled with the remote-desktop scale factor.
6. macOS and Windows builds (MSYS2 / Homebrew): not attempted; the key-code
   and event-loop code paths for them are written but untested.
7. Password is a readable GObject property (also upstream); should be write-only.
