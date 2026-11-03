/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gtk4-frdp-viewer.c — minimal remote-desktop viewer using FrdpDisplay
 *
 * Copyright (C) 2018 Felipe Borges <felipeborges@gnome.org>
 * Copyright (C) 2026 mFat
 *
 * Usage: gtk4-frdp-viewer HOST[:PORT] [USER]
 *   The password is taken from $GTK4_FRDP_PASSWORD if set; otherwise the
 *   server asks for credentials and a dialog collects them.
 */

#include <gtk4-frdp.h>

typedef struct
{
  GtkWindow *window;
  GtkWidget *display;
  gchar     *host;
  guint      port;
} Viewer;

static void
on_certificate_response (GObject      *source,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  FrdpDisplay *display = FRDP_DISPLAY (user_data);
  int          button = gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), result, NULL);

  /* 0 = reject, 1 = accept and remember, 2 = accept for this session only */
  frdp_display_certificate_verify_ex_finish (display, button == 1 ? 2 : button == 2 ? 1 : 0);
  g_object_unref (display);
}

static void
on_needs_certificate_verification (FrdpDisplay *display,
                                   const gchar *host,
                                   guint        port,
                                   const gchar *common_name,
                                   const gchar *subject,
                                   const gchar *issuer,
                                   const gchar *fingerprint,
                                   guint        flags,
                                   Viewer      *viewer)
{
  g_autoptr(GtkAlertDialog) dialog = gtk_alert_dialog_new ("Unknown certificate for %s:%u", host, port);
  g_autofree gchar *detail = g_strdup_printf ("Common name: %s\nIssuer: %s\nFingerprint:\n%s",
                                              common_name, issuer, fingerprint);
  const char *buttons[] = { "Cancel", "Accept once", "Accept and remember", NULL };

  gtk_alert_dialog_set_detail (dialog, detail);
  gtk_alert_dialog_set_buttons (dialog, buttons);
  gtk_alert_dialog_set_cancel_button (dialog, 0);
  /* The session thread waits until frdp_display_certificate_verify_ex_finish()
   * is called; the UI stays responsive meanwhile. */
  gtk_alert_dialog_choose (dialog, viewer->window, NULL,
                           on_certificate_response, g_object_ref (display));
}

static void
on_needs_authentication (FrdpDisplay *display,
                         Viewer      *viewer)
{
  /* A real application would show a dialog here. Answering with empty
   * values cancels the authentication. */
  const gchar *password = g_getenv ("GTK4_FRDP_PASSWORD");
  gchar       *username = NULL;

  g_object_get (display, "username", &username, NULL);
  frdp_display_authenticate_finish (display, username, (gchar *) password, NULL);
  g_free (username);
}

static void
on_error (FrdpDisplay *display,
          const gchar *message,
          Viewer      *viewer)
{
  g_printerr ("Error: %s\n", message);
}

static void
on_connected (FrdpDisplay *display,
              Viewer      *viewer)
{
  g_autofree gchar *title = g_strdup_printf ("%s — gtk4-frdp", viewer->host);

  gtk_window_set_title (viewer->window, title);
  gtk_widget_grab_focus (viewer->display);
}

static void
on_disconnected (FrdpDisplay *display,
                 Viewer      *viewer)
{
  g_print ("Disconnected\n");
  gtk_window_destroy (viewer->window);
}

static void
on_activate (GtkApplication *app,
             Viewer         *viewer)
{
  viewer->window = GTK_WINDOW (gtk_application_window_new (app));
  gtk_window_set_default_size (viewer->window, 1280, 800);
  gtk_window_set_title (viewer->window, "Connecting…");

  viewer->display = frdp_display_new ();
  /* Ask the server to match the widget size; otherwise the desktop is scaled. */
  g_object_set (viewer->display, "allow-resize", TRUE, NULL);
  if (g_object_get_data (G_OBJECT (app), "username") != NULL)
    g_object_set (viewer->display, "username", g_object_get_data (G_OBJECT (app), "username"), NULL);
  if (g_getenv ("GTK4_FRDP_PASSWORD") != NULL)
    g_object_set (viewer->display, "password", g_getenv ("GTK4_FRDP_PASSWORD"), NULL);

  g_signal_connect (viewer->display, "rdp-needs-certificate-verification",
                    G_CALLBACK (on_needs_certificate_verification), viewer);
  g_signal_connect (viewer->display, "rdp-needs-authentication",
                    G_CALLBACK (on_needs_authentication), viewer);
  g_signal_connect (viewer->display, "rdp-error", G_CALLBACK (on_error), viewer);
  g_signal_connect (viewer->display, "rdp-auth-failure", G_CALLBACK (on_error), viewer);
  g_signal_connect (viewer->display, "rdp-connected", G_CALLBACK (on_connected), viewer);
  g_signal_connect (viewer->display, "rdp-disconnected", G_CALLBACK (on_disconnected), viewer);

  gtk_window_set_child (viewer->window, viewer->display);
  gtk_window_present (viewer->window);

  /* Returns immediately; the connection runs on the session thread. */
  frdp_display_open_host (FRDP_DISPLAY (viewer->display), viewer->host, viewer->port);
}

int
main (int   argc,
      char *argv[])
{
  g_autoptr(GtkApplication) app = NULL;
  Viewer  viewer = { 0 };
  gchar  *colon;
  int     ret;

  if (argc < 2) {
    g_printerr ("Usage: %s HOST[:PORT] [USER]\n", argv[0]);
    return 2;
  }

  viewer.host = g_strdup (argv[1]);
  viewer.port = 3389;
  colon = strrchr (viewer.host, ':');
  if (colon != NULL) {
    *colon = '\0';
    viewer.port = (guint) g_ascii_strtoull (colon + 1, NULL, 10);
  }

  app = gtk_application_new ("io.github.gtk4_frdp.Viewer", G_APPLICATION_NON_UNIQUE);
  if (argc > 2)
    g_object_set_data_full (G_OBJECT (app), "username", g_strdup (argv[2]), g_free);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), &viewer);

  ret = g_application_run (G_APPLICATION (app), 1, argv);
  g_free (viewer.host);

  return ret;
}
