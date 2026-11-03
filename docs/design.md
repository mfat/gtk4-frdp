Title: Design and threading model

# Design and threading model

This page explains how gtk4-frdp is put together, for people embedding it
and for contributors. The public API is [class@GtkFrdp.Display]; everything
described below is internal.

## Objects

- **FrdpDisplay** (public) — the `GtkWidget`. Owns one FrdpSession, turns GTK
  input events into RDP input, renders the latest frame in `snapshot()`, and
  emits all signals.
- **FrdpSession** (private) — owns the FreeRDP instance and the session
  thread for one connection at a time.
- **Channel objects** (private) — wrappers for dynamic channels, currently
  Display Control (resolution changes) and the clipboard.

## Clipboard

FreeRDP's clipboard callbacks copy what they receive and queue it to the
main thread; everything else — GTK clipboard calls and every message to the
server — happens there, with a lock guarding the channel against teardown
by the session thread.

- **Server to local:** when the server announces formats, a
  `GdkContentProvider` offering the matching MIME types becomes the local
  clipboard content. Data is only requested from the server when something
  pastes. Requests go one at a time (responses carry no request id) and
  time out after 10 s.
- **Local to server:** a change of the local clipboard's formats is
  announced to the server; when the server asks for data, the local
  clipboard is read asynchronously and the answer sent. The widget's own
  provider is not re-announced.
- **Conversion:** text and HTML through WinPR's `wClipboard`; images through
  `frdp-dib.c` (CF_DIB ↔ `GdkTexture` ↔ PNG/BMP), because WinPR's image
  support depends on its build — Ubuntu's FreeRDP 3.32 has no PNG.
- When the session ends, server data is withdrawn from the clipboard.

`live-clipboard` checks text (Unicode, edited remotely and copied back) and
an image round trip through Paint.

## The session thread

Each connection gets one worker thread that owns FreeRDP. It runs
`freerdp_connect()`, then the event loop
(`freerdp_get_event_handles` / `WaitForMultipleObjects` /
`freerdp_check_event_handles`), all decoding, and finally
`freerdp_disconnect()`. The GTK main thread never waits on the network.

This is the model used by Remmina's RDP plugin and by FreeRDP's own SDL
client. The GTK 3 gtk-frdp this library derives from ran `freerdp_connect()`
on the main thread, which froze the application for the whole connect — 15
seconds for an unreachable host.

| Work | Thread |
|---|---|
| connect, network, decoding, FreeRDP callbacks | session thread |
| GTK calls, signal emission, rendering | main thread |
| keyboard/mouse input (`freerdp_input_send_*`) | main thread, directly |

Input is sent from the main thread, as FreeRDP's SDL client does: FreeRDP's
transport serialises writes with its own lock.

### Getting work to the main thread

FreeRDP callbacks run on the session thread. Anything that touches GTK
(cursor changes, resize, redraw, capability notifications, prompts) is
queued as an idle source on the default main context. Redraws and cursor
updates are coalesced so at most one of each is pending.

The library never uses `g_main_context_invoke()` for this: when no thread
owns the main context at that instant — an application iterating the loop
by hand, or one that has not started its loop yet — `g_main_context_invoke()`
runs the function immediately on the *calling* thread. The test suite found
this: clean-up ran on the session thread, which then tried to join itself.

### The framebuffer

FreeRDP's GDI writes the desktop into `gdi->primary_buffer`. A mutex
(`frame_lock`) is held from FreeRDP's `BeginPaint` to `EndPaint`, and around
`gdi_init`, `gdi_resize` and `gdi_free`. On the main thread, `snapshot()`
takes the same lock and, only when FreeRDP painted since the last frame,
copies the buffer into an immutable `GdkMemoryTexture`. GTK therefore never
reads memory FreeRDP is writing, and an idle desktop costs no copies.

### Prompts

Certificate and credential decisions are FreeRDP callbacks that must return
a value. The session thread queues the request to the main thread and waits
on a condition variable; the widget emits a `rdp-needs-*` signal; the
application answers — immediately or after a dialog — with a `*_finish()`
method, which wakes the session thread. The main loop keeps running
throughout. (gtk-frdp instead spun a nested main loop until the application
answered, which is a re-entrancy hazard.)

### Closing

[method@GtkFrdp.Display.close] is non-blocking. It stops input, wakes a
session thread waiting on a prompt (the prompt counts as rejected), and
calls `freerdp_abort_connect_context()`, which also interrupts a connect in
progress. When the session thread has finished, the main thread joins it,
frees the FreeRDP context and emits [signal@GtkFrdp.Display::rdp-disconnected].

The session thread holds a reference to its session, released on the main
thread after the join, so the last reference can never be dropped on the
session thread.

### Memory

Each session thread gets its own glibc malloc arena, and glibc does not
return freed arena memory to the system by default. Without countermeasures,
resident memory grew by about 20 MB per session although nothing leaked. On
glibc the library calls `malloc_trim(0)` after each session ends.

## Testing

`meson test` runs:

- `header` — the public header compiles on its own; widget creation.
- `widget` — no server needed: creation and freeing, the write-only
  password, a refused connection (fails cleanly, no UI stall, widget freed),
  and `close()` during a hanging connect.
- `live-session` (suite `live`) — skipped unless `GTK4_FRDP_TEST_HOST` is
  set; connects to a real server several times and fails on a missed
  connection, a widget that is not freed, or a UI stall over 250 ms.
- `live-clipboard` (suite `live`) — also needs `xdotool`; text and image
  clipboard in both directions against Windows (Notepad and Paint).
- `keyboard-layout`, `dib` — unit tests for layout mapping/detection and
  the bitmap conversion.
- `live-cursor` (suite `live`) — also needs `xdotool`; checks that the
  remote cursor is scaled together with the desktop.

Live tests run one at a time: they share one server account, and
concurrent logins evict each other.

All tests need a display; run them with `xvfb-run -a meson test`.

## Known gaps

- Clipboard file transfer is not supported; HTML has not been tested
  against Windows applications.
- macOS and Windows builds are written for (key codes, event handles) but
  not yet tested.
