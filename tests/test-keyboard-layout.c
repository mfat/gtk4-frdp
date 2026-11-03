/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 mFat
 *
 * Keyboard layout mapping and detection. Runs without a display; the
 * environment is isolated (in-memory GSettings, empty config directory) so
 * the developer's own desktop settings cannot leak in.
 */
#include <freerdp/locale/keyboard.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

#include "frdp-keyboard-layout.h"

static void
test_from_xkb (void)
{
  g_assert_cmphex (frdp_keyboard_layout_from_xkb ("us", ""), ==, KBD_US);
  g_assert_cmphex (frdp_keyboard_layout_from_xkb ("de", ""), ==, KBD_GERMAN);
  g_assert_cmphex (frdp_keyboard_layout_from_xkb ("fr", NULL), ==, KBD_FRENCH);
  g_assert_cmphex (frdp_keyboard_layout_from_xkb ("us", "dvorak"), ==, KBD_UNITED_STATES_DVORAK);
  /* GNOME's "layout+variant" form */
  g_assert_cmphex (frdp_keyboard_layout_from_xkb ("us+dvorak", NULL), ==, KBD_UNITED_STATES_DVORAK);
  /* Persian: 0 in FreeRDP's table, overridden to Windows' default Persian */
  g_assert_cmphex (frdp_keyboard_layout_from_xkb ("ir", ""), ==, KBD_FARSI);
  g_assert_cmphex (frdp_keyboard_layout_from_xkb ("ir", "pro"), ==, KBD_FARSI);
  /* unknown variant falls back to the base layout */
  g_assert_cmphex (frdp_keyboard_layout_from_xkb ("de", "no-such-variant"), ==, KBD_GERMAN);
  g_assert_cmphex (frdp_keyboard_layout_from_xkb ("no-such-layout", ""), ==, 0);
  g_assert_cmphex (frdp_keyboard_layout_from_xkb (NULL, NULL), ==, 0);
}

static void
check_description (const char *description, const char *layout, const char *variant)
{
  g_autofree char *l = NULL, *v = NULL;

  g_assert_true (frdp_keyboard_layout_xkb_from_description (description, &l, &v));
  g_assert_cmpstr (l, ==, layout);
  g_assert_cmpstr (v, ==, variant);
}

static void
test_from_description (void)
{
  g_autofree char *l = NULL, *v = NULL;

  if (!g_file_test ("/usr/share/X11/xkb/rules/evdev.xml", G_FILE_TEST_EXISTS) &&
      g_getenv ("XKB_CONFIG_ROOT") == NULL) {
    g_test_skip ("xkeyboard-config (evdev.xml) not installed");
    return;
  }

  check_description ("English (US)", "us", "");
  check_description ("German", "de", "");
  check_description ("Persian", "ir", "");
  check_description ("English (Dvorak)", "us", "dvorak");
  g_assert_false (frdp_keyboard_layout_xkb_from_description ("No Such Keyboard", &l, &v));
}

static void
test_detect_environment (void)
{
  const char *source = NULL;

  g_setenv ("XKB_DEFAULT_LAYOUT", "de,us", TRUE);
  g_setenv ("XKB_DEFAULT_VARIANT", "", TRUE);
  g_assert_cmphex (frdp_keyboard_layout_detect (&source), ==, KBD_GERMAN);
  g_assert_cmpstr (source, ==, "environment");
  g_unsetenv ("XKB_DEFAULT_LAYOUT");
  g_unsetenv ("XKB_DEFAULT_VARIANT");
}

static void
test_detect_kde (void)
{
  g_autofree char *path = g_build_filename (g_get_user_config_dir (), "kxkbrc", NULL);
  const char      *source = NULL;

  g_assert_true (g_file_set_contents (path,
                                      "[Layout]\nLayoutList=ir,us\nVariantList=,\nUse=true\n",
                                      -1, NULL));
  {
    guint32 id = frdp_keyboard_layout_detect (&source);
    g_test_message ("kde test: id=0x%08x source=%s", id, source);
    g_assert_cmpstr (source, ==, "kde");
    g_assert_cmphex (id, ==, KBD_FARSI);
  }
  g_unlink (path);
}

static void
test_detect_gnome (void)
{
  GSettingsSchemaSource *src = g_settings_schema_source_get_default ();
  g_autoptr(GSettingsSchema) schema = NULL;
  g_autoptr(GSettings) settings = NULL;
  const char *source = NULL;

  schema = src ? g_settings_schema_source_lookup (src, "org.gnome.desktop.input-sources", TRUE) : NULL;
  if (schema == NULL || !g_settings_schema_has_key (schema, "mru-sources")) {
    g_test_skip ("gsettings-desktop-schemas not installed");
    return;
  }

  settings = g_settings_new_full (schema, NULL, NULL);
  g_assert_true (g_settings_set_value (settings, "mru-sources",
                                       g_variant_new_parsed ("[('xkb', 'us+dvorak'), ('xkb', 'de')]")));
  g_assert_cmphex (frdp_keyboard_layout_detect (&source), ==, KBD_UNITED_STATES_DVORAK);
  g_assert_cmpstr (source, ==, "gnome");

  /* an input method first (no XKB name) falls through to the next source */
  g_assert_true (g_settings_set_value (settings, "mru-sources",
                                       g_variant_new_parsed ("[('ibus', 'anthy')]")));
  g_assert_cmpstr (source = NULL, ==, NULL);
  frdp_keyboard_layout_detect (&source);
  g_assert_cmpstr (source, !=, "gnome");

  g_settings_reset (settings, "mru-sources");
}

static void
test_detect_fallback (void)
{
  const char *source = NULL;
  guint32     id = frdp_keyboard_layout_detect (&source);

  /* No desktop source in the isolated environment: locale or US default. */
  g_assert_true (g_str_equal (source, "locale") || g_str_equal (source, "default"));
  g_assert_cmphex (id, !=, 0);
}

int
main (int argc, char *argv[])
{
  g_autofree char *config = g_dir_make_tmp ("gtk4-frdp-test-XXXXXX", NULL);

  /* Isolate from the developer's desktop before GLib caches anything. */
  g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);
  g_setenv ("XDG_CONFIG_HOME", config, TRUE);
  g_unsetenv ("XKB_DEFAULT_LAYOUT");
  g_unsetenv ("XKB_DEFAULT_VARIANT");
  g_unsetenv ("DISPLAY");
  g_unsetenv ("WAYLAND_DISPLAY");

  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/keyboard-layout/from-xkb", test_from_xkb);
  g_test_add_func ("/keyboard-layout/from-description", test_from_description);
  g_test_add_func ("/keyboard-layout/detect-gnome", test_detect_gnome);
  g_test_add_func ("/keyboard-layout/detect-kde", test_detect_kde);
  g_test_add_func ("/keyboard-layout/detect-environment", test_detect_environment);
  g_test_add_func ("/keyboard-layout/detect-fallback", test_detect_fallback);

  return g_test_run ();
}
