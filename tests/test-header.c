/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 mFat
 *
 * The public header must compile on its own and expose the expected API.
 * Creating the widget needs a display, so that part is skipped (exit 77)
 * when GTK cannot initialise.
 */
#include <gtk4-frdp.h>

int
main (void)
{
  GtkWidget *display;

  g_assert_cmpint (GTK4_FRDP_MAJOR_VERSION, >=, 0);
  g_assert_true (GTK4_FRDP_CHECK_VERSION (0, 1, 0));

  if (!gtk_init_check ())
    return 77;

  display = frdp_display_new ();
  g_assert_true (FRDP_IS_DISPLAY (display));
  g_assert_false (frdp_display_is_open (FRDP_DISPLAY (display)));
  g_assert_nonnull (g_object_class_find_property (G_OBJECT_GET_CLASS (display),
                                                  "last-error-code"));
  g_object_ref_sink (display);
  g_object_unref (display);

  return 0;
}
