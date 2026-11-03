# gtk4-frdp releases

## 0.1.0 (unreleased)

First release as a standalone GTK 4 library, derived from gtk-frdp
(GNOME, GTK 3) at commit 83854a2.

- GTK 4 port of the display widget: event controllers, `snapshot()`
  rendering through `GdkMemoryTexture`, GTK 4 cursors, system-shortcut
  inhibition instead of keyboard grabs.
- Each connection runs on its own session thread; `open_host()` and
  `close()` no longer block, and `close()` cancels a pending connect.
- Certificate and login prompts are answered asynchronously, without a
  nested main loop.
- Public API reduced to `FrdpDisplay`; the session is internal.
  GObject Introspection namespace `GtkFrdp-4.0`, pkg-config
  `gtk4-frdp-4.0`, header `<gtk4-frdp.h>`.
- `password` is write-only.
- FreeRDP 3.11 or newer is required; FreeRDP 2 support was removed.
- New clipboard implementation for GTK 4 (gtk-frdp's GTK 3/FUSE one is
  replaced): text, images and HTML in both directions, fully asynchronous,
  on the session-thread model. Images are converted without depending on
  WinPR's optional PNG support. No file transfer yet.
- Fixed from gtk-frdp: the widget, its session and the FreeRDP context were
  never freed; use-after-free after a failed connection; a certificate
  prompt answered from its signal handler hung; a hidden widget stopped
  processing network events.
- The remote cursor is scaled together with the desktop.
- Keyboard layout detection beyond GNOME: Windows, the GTK keyboard device
  (GTK 4.18+, any desktop), KDE, XKB_DEFAULT_LAYOUT and the system locale,
  using FreeRDP's XKB layout table; new `keyboard-layout` property to set
  it explicitly. Persian (`ir`) maps to Windows' default Persian layout.
- New: test suite (no server needed, plus an optional live suite), API
  reference (gi-docgen), C and Python example viewers.
