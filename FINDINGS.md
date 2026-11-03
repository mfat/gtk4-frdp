# gtk-frdp → GTK4 port spike — 2026-11-03

Question: can gtk-frdp's display + session components be ported to GTK4 and
drive a FreeRDP 3 desktop from Python on Linux, as the base for a native
cross-platform library (the alternative to RDW, see `../rdw/FINDINGS.md`)?

**Answer: yes on Linux. Parity with RDW on every check, better on leaks and
certificates. The blocking-connect blocker is fixed (session thread, below).**
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

## Session thread (the blocker fix)

Pattern taken from the projects that already solved it:
* **Remmina** (`plugins/rdp/rdp_plugin.c`, `rdp_event.c`): one thread owns the
  whole FreeRDP session; UI work is queued to the main thread; synchronous UI
  calls block the worker on a condition variable (`ui_sync_retint`).
* **FreeRDP SDL3 client** (`client/SDL/SDL3/sdl_context.cpp`, 3.32.1): RDP
  thread runs `freerdp_connect()` + the event loop; input is sent from the UI
  thread directly (FreeRDP's transport serialises writes with `WriteLock`).
* **CoffeeRDP** is a fork of that SDL client that additionally runs every
  session in its own process (crash isolation; no embeddable widget).

Implementation (`src/frdp-session.c`):
* worker thread per session: `freerdp_connect()`, event loop, decoding,
  `freerdp_disconnect()`; the GTK thread never waits on the network;
* GTK work from FreeRDP callbacks (cursor, resize, caps, redraw) is
  marshalled with `g_main_context_invoke`, redraws coalesced;
* `frame_lock` held BeginPaint→EndPaint and around gdi init/resize/free;
  `snapshot()` copies under it;
* prompts block only the worker on a `GCond`; the widget emits the signal and
  `*_finish()` wakes the worker — no nested main loop any more;
* `close()` is non-blocking: `freerdp_abort_connect_context()`, context freed
  on the main thread after joining; works mid-connect and mid-prompt;
* `malloc_trim(0)` after each session on glibc (per-thread arenas otherwise
  grew RSS ~20 MB/session without any leak; proven with MALLOC_ARENA_MAX=2).

| Measurement | before (UI-thread connect) | after (session thread) |
|---|---|---|
| Max UI stall, LAN connect | 160–200 ms | 12–42 ms |
| Max UI stall, unreachable host (15 s connect) | **15 050 ms** | **23 ms** |
| `close()` while connecting | n/a (UI frozen) | disconnected 0.00 s later |
| Widgets freed over 10 cycles | 10/10 | 10/10 |
| RSS over 10 cycles | flat ~240 MB | flat 224–284 MB (with arena trim) |
| Idle CPU, settled | ~0.7–0.9 % | ~0.7 %, zero network wake-ups on the UI thread |
| Certificate prompt | nested main loop | worker blocks on GCond; accept-once not stored |

Screenshots `05`/`06`: input and remote resize through the threaded session.

## Remaining work before this could ship

1. Clipboard: text-only path without FUSE; file clipboard Linux-only.
2. Keyboard layout detection reads GNOME settings (Linux only).
3. Cursor is not scaled with the remote-desktop scale factor.
4. macOS and Windows builds (MSYS2 / Homebrew): not attempted; the key-code
   and event-loop code paths for them are written but untested.
5. Password is a readable GObject property (also upstream); should be write-only.
