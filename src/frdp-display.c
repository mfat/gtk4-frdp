/* frdp-display.c
 *
 * Copyright (C) 2018 Felipe Borges <felipeborges@gnome.org>
 *
 * GTK4 port for the sshPilot embedded-RDP spike (2026):
 *  - GtkDrawingArea + GTK3 event vfuncs -> GtkWidget + event controllers
 *  - "draw"/"configure-event" -> snapshot() / size_allocate() forwarded to
 *    the session
 *  - gdk_seat_grab() keyboard grab -> gdk_toplevel_inhibit_system_shortcuts()
 *  - fixed: open_host() leaked a widget reference; the session was never
 *    released; certificate prompts answered synchronously hung forever
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "frdp-display.h"

#include "frdp-session.h"

#include <freerdp/channels/disp.h>

struct _FrdpDisplayPrivate
{
  FrdpSession *session;

  gboolean     allow_resize;
  gboolean     resize_supported;

  gboolean     awaiting_certificate_verification;
  gboolean     awaiting_certificate_change_verification;
  gboolean     awaiting_authentication;

  guint        certificate_verification_value;
  guint        certificate_change_verification_value;

  gboolean     keyboard_grabbed;

  double       last_x;
  double       last_y;
};

G_DEFINE_TYPE_WITH_PRIVATE (FrdpDisplay, frdp_display, GTK_TYPE_WIDGET)

enum
{
  PROP_O = 0,
  PROP_USERNAME,
  PROP_PASSWORD,
  PROP_SCALING,
  PROP_ALLOW_RESIZE,
  PROP_RESIZE_SUPPORTED,
  PROP_DOMAIN
};

enum
{
  RDP_ERROR,
  RDP_CONNECTED,
  RDP_DISCONNECTED,
  RDP_NEEDS_AUTHENTICATION,
  RDP_AUTH_FAILURE,
  RDP_NEEDS_CERTIFICATE_VERIFICATION,
  RDP_NEEDS_CERTIFICATE_CHANGE_VERIFICATION,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

static void frdp_display_set_allow_resize (FrdpDisplay *display,
                                           gboolean     allow_resize);
static void frdp_display_keyboard_grab    (FrdpDisplay *display);
static void frdp_display_keyboard_ungrab  (FrdpDisplay *display);

static gboolean
frdp_display_is_initialized (FrdpDisplay *self)
{
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);

  return priv->session != NULL && frdp_display_is_open (self);
}

/* ---- input ------------------------------------------------------------ */

static gboolean
frdp_display_key_pressed (GtkEventControllerKey *controller,
                          guint                  keyval,
                          guint                  keycode,
                          GdkModifierType        state,
                          gpointer               user_data)
{
  FrdpDisplay *self = FRDP_DISPLAY (user_data);
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);

  if (!frdp_display_is_initialized (self))
    return TRUE;

  frdp_session_send_key (priv->session, keycode, TRUE);

  return TRUE;
}

static void
frdp_display_key_released (GtkEventControllerKey *controller,
                           guint                  keyval,
                           guint                  keycode,
                           GdkModifierType        state,
                           gpointer               user_data)
{
  FrdpDisplay *self = FRDP_DISPLAY (user_data);
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);

  if (!frdp_display_is_initialized (self))
    return;

  frdp_session_send_key (priv->session, keycode, FALSE);
}

static void
frdp_display_motion (GtkEventControllerMotion *controller,
                     double                    x,
                     double                    y,
                     gpointer                  user_data)
{
  FrdpDisplay *self = FRDP_DISPLAY (user_data);
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);

  priv->last_x = x;
  priv->last_y = y;

  if (!frdp_display_is_initialized (self))
    return;

  frdp_session_mouse_event (priv->session, FRDP_MOUSE_EVENT_MOVE, x, y);
}

static guint16
button_flags (guint button)
{
  switch (button) {
  case GDK_BUTTON_PRIMARY:
    return FRDP_MOUSE_EVENT_BUTTON1;
  case GDK_BUTTON_MIDDLE:
    return FRDP_MOUSE_EVENT_BUTTON3;
  case GDK_BUTTON_SECONDARY:
    return FRDP_MOUSE_EVENT_BUTTON2;
  case 8:
    return FRDP_MOUSE_EVENT_BUTTON4;
  case 9:
    return FRDP_MOUSE_EVENT_BUTTON5;
  default:
    return 0;
  }
}

static void
frdp_display_button (GtkGestureClick *gesture,
                     double           x,
                     double           y,
                     gboolean         pressed,
                     gpointer         user_data)
{
  FrdpDisplay *self = FRDP_DISPLAY (user_data);
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);
  guint16 flags;

  if (pressed)
    gtk_widget_grab_focus (GTK_WIDGET (self));

  if (!frdp_display_is_initialized (self))
    return;

  flags = button_flags (gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture)));
  if (flags == 0)
    return;

  if (pressed)
    flags |= FRDP_MOUSE_EVENT_DOWN;

  priv->last_x = x;
  priv->last_y = y;
  frdp_session_mouse_event (priv->session, flags, x, y);
}

static void
frdp_display_pressed (GtkGestureClick *gesture,
                      gint             n_press,
                      double           x,
                      double           y,
                      gpointer         user_data)
{
  frdp_display_button (gesture, x, y, TRUE, user_data);
}

static void
frdp_display_released (GtkGestureClick *gesture,
                       gint             n_press,
                       double           x,
                       double           y,
                       gpointer         user_data)
{
  frdp_display_button (gesture, x, y, FALSE, user_data);
}

static gboolean
frdp_display_scroll (GtkEventControllerScroll *controller,
                     double                    dx,
                     double                    dy,
                     gpointer                  user_data)
{
  FrdpDisplay *self = FRDP_DISPLAY (user_data);
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);

  if (!frdp_display_is_initialized (self))
    return TRUE;

  /* GTK4 reports wheel clicks as +-1.0 deltas and touchpads as smooth
   * deltas; the session's smooth path handles both (1.0 == one notch). */
  frdp_session_mouse_smooth_scroll_event (priv->session,
                                          priv->last_x, priv->last_y,
                                          dx, dy);
  return TRUE;
}

static void
frdp_display_enter (GtkEventControllerMotion *controller,
                    double                    x,
                    double                    y,
                    gpointer                  user_data)
{
  FrdpDisplay *self = FRDP_DISPLAY (user_data);
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);

  if (priv->session == NULL)
    return;

  frdp_session_mouse_pointer (priv->session, TRUE);
  frdp_display_keyboard_grab (self);
}

static void
frdp_display_leave (GtkEventControllerMotion *controller,
                    gpointer                  user_data)
{
  FrdpDisplay *self = FRDP_DISPLAY (user_data);
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);

  if (priv->session == NULL)
    return;

  frdp_session_mouse_pointer (priv->session, FALSE);
  frdp_display_keyboard_ungrab (self);
}

/* ---- rendering / allocation -------------------------------------------- */

static void
frdp_display_snapshot (GtkWidget   *widget,
                       GtkSnapshot *snapshot)
{
  FrdpDisplay *self = FRDP_DISPLAY (widget);
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);

  if (priv->session != NULL)
    frdp_session_snapshot (priv->session, snapshot,
                           gtk_widget_get_width (widget),
                           gtk_widget_get_height (widget));
}

static void
frdp_display_size_allocate (GtkWidget *widget,
                            int        width,
                            int        height,
                            int        baseline)
{
  FrdpDisplay *self = FRDP_DISPLAY (widget);
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);

  GTK_WIDGET_CLASS (frdp_display_parent_class)->size_allocate (widget, width, height, baseline);

  if (priv->session != NULL)
    frdp_session_size_allocated (priv->session, width, height);
}

/* ---- session signals ---------------------------------------------------- */

static void
frdp_display_error (GObject     *source_object,
                    const gchar *message,
                    gpointer     user_data)
{
  g_signal_emit (user_data, signals[RDP_ERROR], 0, message);
}

static void
frdp_display_auth_failure (GObject     *source_object,
                           const gchar *message,
                           gpointer     user_data)
{
  g_signal_emit (user_data, signals[RDP_AUTH_FAILURE], 0, message);
}

static void
frdp_display_disconnected (GObject  *source_object,
                           gpointer  user_data)
{
  FrdpDisplay *self = FRDP_DISPLAY (user_data);
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);

  g_signal_handlers_disconnect_by_func (priv->session, G_CALLBACK (frdp_display_error), self);
  g_signal_handlers_disconnect_by_func (priv->session, G_CALLBACK (frdp_display_disconnected), self);
  g_signal_handlers_disconnect_by_func (priv->session, G_CALLBACK (frdp_display_auth_failure), self);

  g_signal_emit (self, signals[RDP_DISCONNECTED], 0);

  g_debug ("rdp disconnected");
}

static void
frdp_display_open_host_cb (GObject      *source_object,
                           GAsyncResult *result,
                           gpointer      user_data)
{
  FrdpDisplay *self = FRDP_DISPLAY (user_data);
  FrdpSession *session = (FrdpSession*) source_object;
  gboolean success;
  GError  *error = NULL;

  success = frdp_session_connect_finish (session,
                                         result,
                                         &error);

  if (success) {
    g_signal_emit (self, signals[RDP_CONNECTED], 0);

    g_debug ("Connection established");
  } else {
    g_signal_emit (self, signals[RDP_DISCONNECTED], 0);

    g_debug ("Connection failed");
  }

  g_clear_error (&error);
  /* Balances the reference taken in frdp_display_open_host() (upstream leaked it). */
  g_object_unref (self);
}

/* ---- GObject ------------------------------------------------------------ */

static void
frdp_display_get_property (GObject      *object,
                           guint         property_id,
                           GValue       *value,
                           GParamSpec   *pspec)
{
  FrdpDisplay *self = FRDP_DISPLAY (object);
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);
  FrdpSession *session = priv->session;
  gpointer str_property;

  switch (property_id)
    {
      case PROP_USERNAME:
        g_object_get (session, "username", &str_property, NULL);
        g_value_take_string (value, str_property);
        break;
      case PROP_PASSWORD:
        g_object_get (session, "password", &str_property, NULL);
        g_value_take_string (value, str_property);
        break;
      case PROP_DOMAIN:
        g_object_get (session, "domain", &str_property, NULL);
        g_value_take_string (value, str_property);
        break;
      case PROP_SCALING:
        g_object_get (session, "scaling", &str_property, NULL);
        g_value_set_boolean (value, (gboolean)GPOINTER_TO_INT (str_property));
        break;
      case PROP_ALLOW_RESIZE:
        g_value_set_boolean (value, priv->allow_resize);
        break;
      case PROP_RESIZE_SUPPORTED:
        g_value_set_boolean (value, priv->resize_supported);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
frdp_display_set_property (GObject      *object,
                           guint         property_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  FrdpDisplay *self = FRDP_DISPLAY (object);
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);
  FrdpSession *session = priv->session;

  switch (property_id)
    {
      case PROP_USERNAME:
        g_object_set (session, "username", g_value_get_string (value), NULL);
        break;
      case PROP_PASSWORD:
        g_object_set (session, "password", g_value_get_string (value), NULL);
        break;
      case PROP_DOMAIN:
        g_object_set (session, "domain", g_value_get_string (value), NULL);
        break;
      case PROP_SCALING:
        frdp_display_set_scaling (self, g_value_get_boolean (value));
        break;
      case PROP_ALLOW_RESIZE:
        frdp_display_set_allow_resize (self, g_value_get_boolean (value));
        break;
      case PROP_RESIZE_SUPPORTED:
        priv->resize_supported = g_value_get_boolean (value);
        g_object_notify (G_OBJECT (self), "resize-supported");
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
frdp_display_dispose (GObject *object)
{
  FrdpDisplay *self = FRDP_DISPLAY (object);
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);

  /* Upstream never released the session, so every widget leaked its
   * FreeRDP context and framebuffer. */
  if (priv->session != NULL) {
    g_signal_handlers_disconnect_by_data (priv->session, self);
    frdp_session_close (priv->session);
    g_clear_object (&priv->session);
  }

  /* Unblock any nested prompt loop still waiting on this widget. */
  priv->awaiting_authentication = FALSE;
  priv->awaiting_certificate_verification = FALSE;
  priv->awaiting_certificate_change_verification = FALSE;

  G_OBJECT_CLASS (frdp_display_parent_class)->dispose (object);
}

static void
frdp_display_class_init (FrdpDisplayClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  gobject_class->get_property = frdp_display_get_property;
  gobject_class->set_property = frdp_display_set_property;
  gobject_class->dispose = frdp_display_dispose;

  widget_class->snapshot = frdp_display_snapshot;
  widget_class->size_allocate = frdp_display_size_allocate;

  g_object_class_install_property (gobject_class,
                                   PROP_USERNAME,
                                   g_param_spec_string ("username",
                                                        "username",
                                                        "username",
                                                        NULL,
                                                        G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class,
                                   PROP_PASSWORD,
                                   g_param_spec_string ("password",
                                                        "password",
                                                        "password",
                                                        NULL,
                                                        G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class,
                                   PROP_DOMAIN,
                                   g_param_spec_string ("domain",
                                                        "domain",
                                                        "domain",
                                                        NULL,
                                                        G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class,
                                   PROP_SCALING,
                                   g_param_spec_boolean ("scaling",
                                                         "scaling",
                                                         "scaling",
                                                         TRUE,
                                                         G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class,
                                   PROP_ALLOW_RESIZE,
                                   g_param_spec_boolean ("allow-resize",
                                                         "allow-resize",
                                                         "allow-resize",
                                                         FALSE,
                                                         G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class,
                                   PROP_RESIZE_SUPPORTED,
                                   g_param_spec_boolean ("resize-supported",
                                                         "resize-supported",
                                                         "resize-supported",
                                                         FALSE,
                                                         G_PARAM_READWRITE));

  signals[RDP_ERROR] = g_signal_new ("rdp-error",
                                     G_TYPE_FROM_CLASS (klass),
                                     G_SIGNAL_RUN_LAST,
                                     0, NULL, NULL, NULL,
                                     G_TYPE_NONE, 1,
                                     G_TYPE_STRING);

  signals[RDP_CONNECTED] = g_signal_new ("rdp-connected",
                                         G_TYPE_FROM_CLASS (klass),
                                         G_SIGNAL_RUN_LAST,
                                         0, NULL, NULL, NULL,
                                         G_TYPE_NONE, 0);

  signals[RDP_DISCONNECTED] = g_signal_new ("rdp-disconnected",
                                            G_TYPE_FROM_CLASS (klass),
                                            G_SIGNAL_RUN_LAST,
                                            0, NULL, NULL, NULL,
                                            G_TYPE_NONE, 0);

  signals[RDP_NEEDS_AUTHENTICATION] = g_signal_new ("rdp-needs-authentication",
                                                    G_TYPE_FROM_CLASS (klass),
                                                    G_SIGNAL_RUN_LAST,
                                                    0, NULL, NULL, NULL,
                                                    G_TYPE_NONE, 0);

  signals[RDP_AUTH_FAILURE] = g_signal_new ("rdp-auth-failure",
                                            G_TYPE_FROM_CLASS (klass),
                                            G_SIGNAL_RUN_LAST,
                                            0, NULL, NULL, NULL,
                                            G_TYPE_NONE, 1,
                                            G_TYPE_STRING);

  signals[RDP_NEEDS_CERTIFICATE_VERIFICATION] = g_signal_new ("rdp-needs-certificate-verification",
                                                              G_TYPE_FROM_CLASS (klass),
                                                              G_SIGNAL_RUN_LAST,
                                                              0, NULL, NULL, NULL,
                                                              G_TYPE_NONE, 7,
                                                              G_TYPE_STRING,
                                                              G_TYPE_UINT,
                                                              G_TYPE_STRING,
                                                              G_TYPE_STRING,
                                                              G_TYPE_STRING,
                                                              G_TYPE_STRING,
                                                              G_TYPE_UINT);

  signals[RDP_NEEDS_CERTIFICATE_CHANGE_VERIFICATION] = g_signal_new ("rdp-needs-certificate-change-verification",
                                                                     G_TYPE_FROM_CLASS (klass),
                                                                     G_SIGNAL_RUN_LAST,
                                                                     0, NULL, NULL, NULL,
                                                                     G_TYPE_NONE, 10,
                                                                     G_TYPE_STRING,
                                                                     G_TYPE_UINT,
                                                                     G_TYPE_STRING,
                                                                     G_TYPE_STRING,
                                                                     G_TYPE_STRING,
                                                                     G_TYPE_STRING,
                                                                     G_TYPE_STRING,
                                                                     G_TYPE_STRING,
                                                                     G_TYPE_STRING,
                                                                     G_TYPE_UINT);
}

static void
frdp_display_init (FrdpDisplay *self)
{
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);
  GtkEventController *key, *motion, *scroll;
  GtkGesture *click;

  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  key = gtk_event_controller_key_new ();
  g_signal_connect (key, "key-pressed", G_CALLBACK (frdp_display_key_pressed), self);
  g_signal_connect (key, "key-released", G_CALLBACK (frdp_display_key_released), self);
  gtk_widget_add_controller (GTK_WIDGET (self), key);

  motion = gtk_event_controller_motion_new ();
  g_signal_connect (motion, "motion", G_CALLBACK (frdp_display_motion), self);
  g_signal_connect (motion, "enter", G_CALLBACK (frdp_display_enter), self);
  g_signal_connect (motion, "leave", G_CALLBACK (frdp_display_leave), self);
  gtk_widget_add_controller (GTK_WIDGET (self), motion);

  click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), 0); /* all buttons */
  g_signal_connect (click, "pressed", G_CALLBACK (frdp_display_pressed), self);
  g_signal_connect (click, "released", G_CALLBACK (frdp_display_released), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));

  scroll = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
  g_signal_connect (scroll, "scroll", G_CALLBACK (frdp_display_scroll), self);
  gtk_widget_add_controller (GTK_WIDGET (self), scroll);

  priv->session = frdp_session_new (self);

  g_object_bind_property (priv->session, "monitor-layout-supported", self, "resize-supported", 0);
}

/**
 * frdp_display_open_host:
 * @display: (transfer none): the RDP display widget
 * @host: (transfer none): the hostname or IP address
 * @port: the service name or port number
 *
 * Opens a TCP connection to the given @host litening on
 * @port.
 */
void
frdp_display_open_host (FrdpDisplay  *display,
                        const gchar  *host,
                        guint         port)
{
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (display);

  g_return_if_fail (host != NULL);

  g_signal_connect (priv->session, "rdp-error",
                    G_CALLBACK (frdp_display_error),
                    display);
  g_signal_connect (priv->session, "rdp-disconnected",
                    G_CALLBACK (frdp_display_disconnected),
                    display);
  g_signal_connect (priv->session, "rdp-auth-failure",
                    G_CALLBACK (frdp_display_auth_failure),
                    display);

  frdp_session_connect (priv->session,
                        host,
                        port,
                        NULL, // TODO: Cancellable
                        frdp_display_open_host_cb,
                        g_object_ref (display));

  g_debug ("Connecting to %s…", host);
}

/**
 * frdp_display_is_open:
 * @display: (transfer none): the RDP display widget
 *
 * Check if the connection for the display is currently open
 *
 * Returns: TRUE if open, FALSE if closing/closed
 */
gboolean
frdp_display_is_open (FrdpDisplay *display)
{
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (display);

  return priv->session != NULL && frdp_session_is_open (priv->session);
}

/**
 * frdp_display_close:
 * @display: (transfer none): the RDP display widget
 *
 * Request the closing of the RDP session.
 */
void
frdp_display_close (FrdpDisplay *display)
{
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (display);

  if (priv->session != NULL)
    frdp_session_close (priv->session);
}

/**
 * frdp_display_set_scaling:
 * @display: (transfer none): the RDP display widget
 * @scaling: TRUE to scale the desktop to fit, FALSE otherwise
 *
 * Set whether the remote desktop contents is automatically
 * scaled to fit the available widget size, or whether it will
 * be rendered at 1:1 size
 */
void
frdp_display_set_scaling (FrdpDisplay *display,
                          gboolean     scaling)
{
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (display);
  gboolean            scaling_old;

  g_object_get (priv->session, "scaling", &scaling_old, NULL);
  if (scaling != scaling_old) {
    g_object_set (priv->session, "scaling", scaling, NULL);

    if (scaling) {
      gtk_widget_set_size_request (GTK_WIDGET (display), -1, -1);

      gtk_widget_set_halign (GTK_WIDGET (display), GTK_ALIGN_FILL);
      gtk_widget_set_valign (GTK_WIDGET (display), GTK_ALIGN_FILL);
    }

    gtk_widget_queue_resize (GTK_WIDGET (display));
    gtk_widget_queue_draw (GTK_WIDGET (display));
  }
}

static void
frdp_display_set_allow_resize (FrdpDisplay *display,
                               gboolean     allow_resize)
{
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (display);

  priv->allow_resize = allow_resize;

  if (allow_resize) {
    gtk_widget_set_size_request (GTK_WIDGET (display), -1, -1);

    gtk_widget_set_halign (GTK_WIDGET (display), GTK_ALIGN_FILL);
    gtk_widget_set_valign (GTK_WIDGET (display), GTK_ALIGN_FILL);
  }

  gtk_widget_queue_draw (GTK_WIDGET (display));
}

/*
 * frdp_display_new:
 *
 * Create a new widget capable of connecting to a RDP server
 * and displaying its contents
 *
 * The widget will be initially be in a disconnected state
 *
 * Returns: (transfer full): the new RDP display widget
 */
GtkWidget *frdp_display_new (void)
{
  return GTK_WIDGET (g_object_new (FRDP_TYPE_DISPLAY, NULL));
}

gboolean
frdp_display_authenticate (FrdpDisplay  *self,
                           gchar       **username,
                           gchar       **password,
                           gchar       **domain)
{
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);
  GMainContext       *context;

  priv->awaiting_authentication = TRUE;

  g_signal_emit (self, signals[RDP_NEEDS_AUTHENTICATION], 0);

  context = g_main_context_default ();

  while (priv->awaiting_authentication)
    g_main_context_iteration (context, TRUE);

  *username = *password = *domain = NULL;
  g_object_get (priv->session,
                "username", username,
                "password", password,
                "domain", domain,
                NULL);

  if (*username != NULL && *username[0] == '\0' &&
      *password != NULL && *password[0] == '\0' &&
      *domain != NULL && *domain[0] == '\0')
    return FALSE;

  return TRUE;
}

/*
 * frdp_display_authenticate_finish:
 * @display: (transfer none): the RDP display widget
 * @username: (transfer none): username for the connection
 * @password: (transfer none): password for the connection
 * @domain: (transfer none): optional domain for the connection
 *
 * This function finishes authentication which started in
 * frdp_display_authenticate() and stores given authentication
 * credentials into FrdpSession so that frdp_display_authenticate()
 * can pick them up later and pass them to FreeRDP.
 *
 */
void
frdp_display_authenticate_finish (FrdpDisplay *self,
                                  gchar       *username,
                                  gchar       *password,
                                  gchar       *domain)
{
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);

  g_object_set (priv->session,
                "username", username,
                "password", password,
                "domain", domain,
                NULL);

  priv->awaiting_authentication = FALSE;
}

guint
frdp_display_certificate_verify_ex (FrdpDisplay *display,
                                    const gchar *host,
                                    guint16      port,
                                    const gchar *common_name,
                                    const gchar *subject,
                                    const gchar *issuer,
                                    const gchar *fingerprint,
                                    guint32      flags)
{
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (display);
  GMainContext       *context;

  /* Set before emitting: upstream set it after, so a handler that
   * answered synchronously had its answer overwritten and hung forever. */
  priv->awaiting_certificate_verification = TRUE;
  priv->certificate_verification_value = 0;

  g_signal_emit (display,
                 signals[RDP_NEEDS_CERTIFICATE_VERIFICATION],
                 0,
                 host,
                 port,
                 common_name,
                 subject,
                 issuer,
                 fingerprint,
                 flags);

  context = g_main_context_default ();

  while (priv->awaiting_certificate_verification)
    g_main_context_iteration (context, TRUE);

  return priv->certificate_verification_value;
}

guint
frdp_display_certificate_change_verify_ex (FrdpDisplay *display,
                                           const gchar *host,
                                           guint16      port,
                                           const gchar *common_name,
                                           const gchar *subject,
                                           const gchar *issuer,
                                           const gchar *fingerprint,
                                           const gchar *old_subject,
                                           const gchar *old_issuer,
                                           const gchar *old_fingerprint,
                                           guint32      flags)
{
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (display);
  GMainContext       *context;

  priv->awaiting_certificate_change_verification = TRUE;
  priv->certificate_change_verification_value = 0;

  g_signal_emit (display, signals[RDP_NEEDS_CERTIFICATE_CHANGE_VERIFICATION], 0,
                 host,
                 port,
                 common_name,
                 subject,
                 issuer,
                 fingerprint,
                 old_subject,
                 old_issuer,
                 old_fingerprint,
                 flags);

  context = g_main_context_default ();

  while (priv->awaiting_certificate_change_verification)
    g_main_context_iteration (context, TRUE);

  return priv->certificate_change_verification_value;
}

/**
 * frdp_display_certificate_verify_ex_finish:
 * @display: (transfer none): the RDP display widget
 * @verification: verification value (1 - accept and store the certificate,
 *                                    2 - accept the certificate for this session only
 *                                    0 - otherwise)
 *
 * Finishes verification requested by FreeRDP.
 */
void
frdp_display_certificate_verify_ex_finish (FrdpDisplay *display,
                                           guint        verification)
{
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (display);

  if (verification <= 2) {
    priv->certificate_verification_value = verification;
  }
  else {
    priv->certificate_verification_value = 0;
    g_warning ("Verification value is out of allowed values.");
  }
  priv->awaiting_certificate_verification = FALSE;
}

/**
 * frdp_display_certificate_change_verify_ex_finish:
 * @display: (transfer none): the RDP display widget
 * @verification: verification value (1 - accept and store the certificate,
 *                                    2 - accept the certificate for this session only
 *                                    0 - otherwise)
 *
 * Finishes verification requested by FreeRDP.
 */
void
frdp_display_certificate_change_verify_ex_finish (FrdpDisplay *display,
                                                  guint        verification)
{
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (display);

  if (verification <= 2) {
    priv->certificate_change_verification_value = verification;
  }
  else {
    priv->certificate_change_verification_value = 0;
    g_warning ("Verification value is out of allowed values.");
  }
  priv->awaiting_certificate_change_verification = FALSE;
}

static GdkToplevel *
frdp_display_get_toplevel (FrdpDisplay *display)
{
  GtkNative  *native = gtk_widget_get_native (GTK_WIDGET (display));
  GdkSurface *surface;

  if (native == NULL)
    return NULL;

  surface = gtk_native_get_surface (native);

  return GDK_IS_TOPLEVEL (surface) ? GDK_TOPLEVEL (surface) : NULL;
}

/* GTK4 has no keyboard grab. Inhibiting the compositor's system shortcuts
 * while the pointer is over the desktop is the supported equivalent (Super,
 * Alt+Tab etc. then reach the remote session). */
static void
frdp_display_keyboard_grab (FrdpDisplay *display)
{
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (display);
  GdkToplevel        *toplevel = frdp_display_get_toplevel (display);

  if (toplevel == NULL)
    return;

  gdk_toplevel_inhibit_system_shortcuts (toplevel, NULL);
  priv->keyboard_grabbed = TRUE;
}

static void
frdp_display_keyboard_ungrab (FrdpDisplay *display)
{
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (display);
  GdkToplevel        *toplevel = frdp_display_get_toplevel (display);

  if (toplevel != NULL && priv->keyboard_grabbed)
    gdk_toplevel_restore_system_shortcuts (toplevel);

  priv->keyboard_grabbed = FALSE;
}

gboolean
frdp_display_is_keyboard_grabbed (FrdpDisplay *self)
{
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);

  return priv->keyboard_grabbed;
}
