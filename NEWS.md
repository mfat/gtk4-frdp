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
- Clipboard is not available yet: gtk-frdp's GTK 3 clipboard channel is
  kept in the tree but not built until it is ported to GTK 4.
- Fixed from gtk-frdp: the widget, its session and the FreeRDP context were
  never freed; use-after-free after a failed connection; a certificate
  prompt answered from its signal handler hung; a hidden widget stopped
  processing network events.
- The remote cursor is scaled together with the desktop.
- New: test suite (no server needed, plus an optional live suite), API
  reference (gi-docgen), C and Python example viewers.
