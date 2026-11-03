/* frdp-session.c
 *
 * Copyright (C) 2018 Felipe Borges <felipeborges@gnome.org>
 * Copyright (C) 2019 Armin Novak <akallabeth@posteo.net>
 * Copyright (C) 2023 Marek Kasik <mkasik@redhat.com>
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

#include <errno.h>
#include <freerdp/input.h>
#include <freerdp/locale/keyboard.h>
#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/gdi/video.h>
#include <freerdp/gdi/gfx.h>
#include <freerdp/client/channels.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/client/channels.h>
#include <freerdp/client/rdpei.h>
#include <freerdp/client/tsmf.h>
#include <freerdp/client/rail.h>
#include <freerdp/client/cliprdr.h>
#include <freerdp/client/rdpgfx.h>
#include <freerdp/client/encomsp.h>
#include <freerdp/client/disp.h>
#include <freerdp/client/geometry.h>
#include <freerdp/client/video.h>
#include <freerdp/channels/disp.h>
#include <gdk/gdk.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <math.h>

#include "frdp-session.h"
#include "frdp-context.h"
#include "frdp-channel-display-control.h"
#ifdef FRDP_WITH_CLIPBOARD
#include "frdp-channel-clipboard.h"
#endif

#define SELECT_TIMEOUT 20

/* Spike instrumentation: FRDP_SPIKE_STATS=1 prints loop/frame rates every 5 s. */
static struct { guint loops, timeouts, signaled, uploads; gint64 since; } spike_stats;

static void
spike_stats_tick (void)
{
  gint64 now = g_get_monotonic_time ();

  if (spike_stats.since == 0)
    spike_stats.since = now;
  if (now - spike_stats.since >= 5 * G_USEC_PER_SEC) {
    if (g_getenv ("FRDP_SPIKE_STATS") != NULL)
      g_printerr ("frdp-stats: per 5s dispatches=%u unused=%u draws_queued=%u texture_uploads=%u\n",
                  spike_stats.loops, spike_stats.timeouts, spike_stats.signaled, spike_stats.uploads);
    memset (&spike_stats, 0, sizeof spike_stats);
    spike_stats.since = now;
  }
}
#define FRDP_CONNECTION_THREAD_MAX_ERRORS 10

#ifdef HAVE_FREERDP3
#define CONST_QUALIFIER const
#else
#define CONST_QUALIFIER
#endif

struct frdp_pointer
{
	rdpPointer pointer;
	GdkTexture *texture;
};
typedef struct frdp_pointer frdpPointer;

struct _FrdpSessionPrivate
{
  freerdp      *freerdp_session;

  GtkWidget    *display;
  /* GTK4: the framebuffer is uploaded as an immutable texture per frame
   * instead of wrapping gdi->primary_buffer in a cairo surface. */
  GdkTexture   *frame;
  gboolean      frame_dirty;
  guint         resize_timeout_id;
  int           alloc_width;
  int           alloc_height;
  gboolean scaling;
  double scale;
  double offset_x;
  double offset_y;

  guint update_id;
  guint close_idle_id;

  gboolean is_connected;

  gchar *hostname;
  gchar *username;
  gchar *password;
  gchar *domain;
  guint  port;

  gboolean show_cursor;
  gboolean cursor_null;
  frdpPointer *cursor;

  /* Channels */
  FrdpChannelDisplayControl *display_control_channel;
#ifdef FRDP_WITH_CLIPBOARD
  FrdpChannelClipboard      *clipboard_channel;
#endif
  gboolean                   monitor_layout_supported;
};

G_DEFINE_TYPE_WITH_PRIVATE (FrdpSession, frdp_session, G_TYPE_OBJECT)

enum
{
  PROP_0 = 0,
  PROP_HOSTNAME,
  PROP_PORT,
  PROP_USERNAME,
  PROP_PASSWORD,
  PROP_DISPLAY,
  PROP_SCALING,
  PROP_MONITOR_LAYOUT_SUPPORTED,
  PROP_DOMAIN
};

enum
{
  RDP_ERROR,
  RDP_CONNECTED,
  RDP_DISCONNECTED,
  RDP_AUTH_FAILURE,
  RDP_CHANNEL_CONNECTED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

static void
frdp_session_update_mouse_pointer (FrdpSession  *self)
{
  FrdpSessionPrivate *priv = self->priv;
  GdkCursor *cursor;

  if (priv->display == NULL)
    return;

  if (priv->show_cursor && priv->cursor_null) {
    /* Server hides the pointer. */
    cursor = gdk_cursor_new_from_name ("none", NULL);
  } else if (!priv->show_cursor || !priv->cursor || !priv->cursor->texture) {
    /* No cursor set or none to show */
    cursor = gdk_cursor_new_from_name ("default", NULL);
  } else {
    /* GTK4 scales cursor textures for the surface scale itself; the
     * remote-desktop scale factor is not applied here (spike scope). */
    cursor = gdk_cursor_new_from_texture (priv->cursor->texture,
                                          priv->cursor->pointer.xPos,
                                          priv->cursor->pointer.yPos,
                                          NULL);
  }

  gtk_widget_set_cursor (priv->display, cursor);
  g_clear_object (&cursor);
}

static BOOL
frdp_pointer_new (rdpContext *context,
                  rdpPointer *pointer)
{
  frdpContext *fcontext = (frdpContext *) context;
  frdpPointer *fpointer = (frdpPointer *) pointer;
  gsize        stride;
  guint8      *data;
  GBytes      *bytes;

  if (fcontext == NULL || fpointer == NULL || pointer->width == 0 || pointer->height == 0)
    return FALSE;

  stride = (gsize) pointer->width * 4;
  /* Zeroed: freerdp_image_copy_from_pointer_data does not initialise the
   * buffer (FreeRDP issue 5061). */
  data = g_malloc0 (stride * pointer->height);

  if (!freerdp_image_copy_from_pointer_data (data, PIXEL_FORMAT_BGRA32,
                                             stride, 0, 0, pointer->width,
                                             pointer->height,
                                             pointer->xorMaskData,
                                             pointer->lengthXorMask,
                                             pointer->andMaskData,
                                             pointer->lengthAndMask,
                                             pointer->xorBpp,
                                             &context->gdi->palette)) {
    g_free (data);
    return FALSE;
  }

  bytes = g_bytes_new_take (data, stride * pointer->height);
  fpointer->texture = gdk_memory_texture_new (pointer->width, pointer->height,
                                              GDK_MEMORY_B8G8R8A8,
                                              bytes, stride);
  g_bytes_unref (bytes);

  return TRUE;
}

static void
frdp_pointer_free (rdpContext *context,
                   rdpPointer *pointer)
{
  frdpPointer *fpointer = (frdpPointer *) pointer;

  if (fpointer != NULL)
    g_clear_object (&fpointer->texture);
}

static BOOL
frdp_pointer_set (rdpContext *context,
                  rdpPointer *pointer)
{
  frdpContext *fcontext = (frdpContext *) context;
  frdpPointer *fpointer = (frdpPointer *) pointer;
  FrdpSessionPrivate *priv = fcontext->self->priv;

  priv->cursor = fpointer;
  priv->cursor_null = FALSE;

  frdp_session_update_mouse_pointer (fcontext->self);

  return TRUE;
}

static BOOL
frdp_pointer_set_null (rdpContext *context)
{
  frdpContext *fcontext = (frdpContext *) context;
  FrdpSessionPrivate *priv = fcontext->self->priv;

  priv->cursor = NULL;
  priv->cursor_null = TRUE;

  frdp_session_update_mouse_pointer (fcontext->self);

  return TRUE;
}

static BOOL
frdp_pointer_set_default (rdpContext *context)
{
  frdpContext *fcontext = (frdpContext *) context;
  FrdpSessionPrivate *priv = fcontext->self->priv;

  priv->cursor = NULL;
  priv->cursor_null = FALSE;

  frdp_session_update_mouse_pointer (fcontext->self);

  return TRUE;
}

static BOOL
frdp_pointer_set_position (rdpContext *context,
                           UINT32      x,
                           UINT32      y)
{
  return TRUE;
}

static void
frdp_register_pointer (rdpGraphics *graphics)
{
  rdpPointer pointer = { 0 };

  pointer.size = sizeof (frdpPointer);
  pointer.New = frdp_pointer_new;
  pointer.Free = frdp_pointer_free;
  pointer.Set = frdp_pointer_set;
  pointer.SetNull = frdp_pointer_set_null;
  pointer.SetDefault = frdp_pointer_set_default;
  pointer.SetPosition = frdp_pointer_set_position;
  graphics_register_pointer (graphics, &pointer);
}

static rdpGdi *
frdp_session_get_gdi (FrdpSession *self)
{
  FrdpSessionPrivate *priv = self->priv;

  if (priv->freerdp_session == NULL || priv->freerdp_session->context == NULL)
    return NULL;

  return priv->freerdp_session->context->gdi;
}

/* Scale/offset used both for drawing and for mapping pointer coordinates. */
static void
frdp_session_update_scale (FrdpSession *self)
{
  FrdpSessionPrivate *priv = self->priv;
  rdpGdi             *gdi = frdp_session_get_gdi (self);
  double              width = priv->alloc_width, height = priv->alloc_height;
  double              widget_ratio, server_ratio;

  if (gdi == NULL || gdi->width == 0 || gdi->height == 0 || !priv->scaling ||
      width <= 0 || height <= 0) {
    priv->scale = 1.0;
    priv->offset_x = priv->offset_y = 0.0;
    return;
  }

  widget_ratio = width / height;
  server_ratio = (double) gdi->width / gdi->height;

  if (widget_ratio > server_ratio)
    priv->scale = height / gdi->height;
  else
    priv->scale = width / gdi->width;

  priv->offset_x = (width - gdi->width * priv->scale) / 2.0;
  priv->offset_y = (height - gdi->height * priv->scale) / 2.0;
}

static void
frdp_session_update_size_request (FrdpSession *self)
{
  FrdpSessionPrivate *priv = self->priv;
  rdpGdi             *gdi = frdp_session_get_gdi (self);
  gboolean            allow_resize = FALSE;

  if (priv->display == NULL)
    return;

  g_object_get (G_OBJECT (priv->display), "allow-resize", &allow_resize, NULL);

  if (priv->scaling || allow_resize || gdi == NULL)
    gtk_widget_set_size_request (priv->display, -1, -1);
  else
    gtk_widget_set_size_request (priv->display, gdi->width, gdi->height);
}

static gboolean
frdp_desktop_resize (rdpContext *context)
{
  FrdpSession *self = ((frdpContext *) context)->self;
  rdpGdi      *gdi = context->gdi;

  if (gdi_resize (gdi,
                  freerdp_settings_get_uint32 (context->settings, FreeRDP_DesktopWidth),
                  freerdp_settings_get_uint32 (context->settings, FreeRDP_DesktopHeight))) {
    frdp_session_update_size_request (self);
    frdp_session_update_scale (self);
    self->priv->frame_dirty = TRUE;
    return TRUE;
  } else {
    return FALSE;
  }
}

static void
frdp_session_request_remote_resize (FrdpSession *self)
{
  FrdpSessionPrivate *priv = self->priv;
  rdpSettings        *settings;
  guint32             request_width, request_height;

  if (priv->freerdp_session == NULL || priv->display_control_channel == NULL ||
      priv->alloc_width <= 0 || priv->alloc_height <= 0)
    return;

  settings = priv->freerdp_session->context->settings;
  request_width = priv->alloc_width;
  request_height = priv->alloc_height;

  /* Round width to even number */
  if (request_width % 2 == 1)
    request_width--;

  /* Only request resize if the rounded dimensions differ from the current
   * desktop size; prevents resize loops with odd dimensions. */
  if (freerdp_settings_get_uint32 (settings, FreeRDP_DesktopWidth) != request_width ||
      freerdp_settings_get_uint32 (settings, FreeRDP_DesktopHeight) != request_height)
    frdp_channel_display_control_resize_display (priv->display_control_channel,
                                                 request_width,
                                                 request_height);
}

static gboolean
frdp_session_resize_timeout (gpointer user_data)
{
  FrdpSession *self = user_data;

  self->priv->resize_timeout_id = 0;
  frdp_session_request_remote_resize (self);

  return G_SOURCE_REMOVE;
}

/* Replaces the GTK3 "configure-event" handler. Uses the widget's own
 * allocation instead of assuming a GtkScrolledWindow ancestor, and
 * debounces remote resizes so a window drag does not renegotiate the
 * monitor layout on every allocation. */
void
frdp_session_size_allocated (FrdpSession *self,
                             int          width,
                             int          height)
{
  FrdpSessionPrivate *priv = self->priv;
  gboolean            allow_resize = FALSE;

  priv->alloc_width = width;
  priv->alloc_height = height;

  frdp_session_update_scale (self);

  if (priv->display == NULL || priv->freerdp_session == NULL)
    return;

  g_object_get (G_OBJECT (priv->display), "allow-resize", &allow_resize, NULL);
  if (!allow_resize)
    return;

  if (priv->resize_timeout_id != 0)
    g_source_remove (priv->resize_timeout_id);
  priv->resize_timeout_id = g_timeout_add (200, frdp_session_resize_timeout, self);
}

static void
frdp_session_resize_supported_changed (FrdpDisplay *display,
                                       GParamSpec  *pspec,
                                       gpointer     user_data)
{
  FrdpSession        *self = user_data;
  gboolean            resize_supported, allow_resize;

  g_object_get (G_OBJECT (display),
                "resize-supported", &resize_supported,
                "allow-resize", &allow_resize,
                NULL);

  if (resize_supported && allow_resize)
    frdp_session_request_remote_resize (self);
}

static void
frdp_session_set_scaling (FrdpSession *self,
                          gboolean     scaling)
{
  self->priv->scaling = scaling;
  frdp_session_update_scale (self);
  frdp_session_update_size_request (self);
}

/* Replaces the GTK3 "draw" handler. The framebuffer is copied into an
 * immutable GdkMemoryTexture at most once per frame (only when FreeRDP
 * painted since the last one), so GTK never reads a buffer FreeRDP is
 * writing. */
void
frdp_session_snapshot (FrdpSession *self,
                       GtkSnapshot *snapshot,
                       int          width,
                       int          height)
{
  FrdpSessionPrivate *priv = self->priv;
  rdpGdi             *gdi = frdp_session_get_gdi (self);

  if (!priv->is_connected || gdi == NULL || gdi->primary_buffer == NULL)
    return;

  if (priv->frame == NULL || priv->frame_dirty ||
      gdk_texture_get_width (priv->frame) != (int) gdi->width ||
      gdk_texture_get_height (priv->frame) != (int) gdi->height) {
    GBytes *bytes = g_bytes_new (gdi->primary_buffer, (gsize) gdi->stride * gdi->height);

    g_clear_object (&priv->frame);
    spike_stats.uploads++;
    priv->frame = gdk_memory_texture_new (gdi->width, gdi->height,
                                          GDK_MEMORY_B8G8R8X8,
                                          bytes, gdi->stride);
    g_bytes_unref (bytes);
    priv->frame_dirty = FALSE;
  }

  if (priv->scaling) {
    graphene_rect_t rect = GRAPHENE_RECT_INIT (priv->offset_x, priv->offset_y,
                                               gdi->width * priv->scale,
                                               gdi->height * priv->scale);
    gtk_snapshot_append_scaled_texture (snapshot, priv->frame,
                                        GSK_SCALING_FILTER_LINEAR, &rect);
  } else {
    graphene_rect_t rect = GRAPHENE_RECT_INIT (0, 0, gdi->width, gdi->height);
    gtk_snapshot_append_texture (snapshot, priv->frame, &rect);
  }
}

/*
 * Return 1 to accept and store a certificate, 2 to accept
 * a certificate only for this session, 0 otherwise.
 */
static guint
frdp_certificate_verify_ex (freerdp     *freerdp_session,
                            const gchar *host,
                            guint16      port,
                            const gchar *common_name,
                            const gchar *subject,
                            const gchar *issuer,
                            const gchar *fingerprint,
                            guint32      flags)
{
  FrdpSession *self = ((frdpContext *) freerdp_session->context)->self;

  if (self->priv->display == NULL)
    return 0; /* widget gone: refuse */

  return frdp_display_certificate_verify_ex (FRDP_DISPLAY (self->priv->display),
                                             host,
                                             port,
                                             common_name,
                                             subject,
                                             issuer,
                                             fingerprint,
                                             flags);
}

static guint
frdp_changed_certificate_verify_ex (freerdp     *freerdp_session,
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
  FrdpSession *self = ((frdpContext *) freerdp_session->context)->self;

  if (self->priv->display == NULL)
    return 0; /* widget gone: refuse */

  return frdp_display_certificate_change_verify_ex (FRDP_DISPLAY (self->priv->display),
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
}

static gboolean
frdp_authenticate_ex (freerdp          *freerdp_session,
                      gchar           **username,
                      gchar           **password,
                      gchar           **domain,
                      rdp_auth_reason   reason)
{
  FrdpSession *self = ((frdpContext *) freerdp_session->context)->self;

  if (self->priv->display == NULL)
    return 0; /* widget gone: refuse */

  return frdp_display_authenticate (FRDP_DISPLAY (self->priv->display),
                                    username,
                                    password,
                                    domain);
}

static void
caps_set (FrdpChannelDisplayControl *channel,
          gpointer                   user_data)
{
  FrdpSession *session = user_data;

  g_object_set (G_OBJECT (session), "monitor-layout-supported", TRUE, NULL);
}

static void
frdp_on_channel_connected_event_handler (void                                      *context,
                                         CONST_QUALIFIER ChannelConnectedEventArgs *e)
{
  frdpContext        *ctx = (frdpContext *) context;
  FrdpSession        *session = ctx->self;
  FrdpSessionPrivate *priv = frdp_session_get_instance_private (session);

  if (strcmp (e->name, RDPEI_DVC_CHANNEL_NAME) == 0) {
    // TODO Touch input redirection
  } else if (strcmp (e->name, DISP_DVC_CHANNEL_NAME) == 0) {
    g_clear_object (&priv->display_control_channel);

    priv->display_control_channel = g_object_new (FRDP_TYPE_CHANNEL_DISPLAY_CONTROL,
                                                  "session", session,
                                                  "display-client-context", (DispClientContext *) e->pInterface,
                                                  NULL);
    g_signal_connect (priv->display_control_channel, "caps-set", G_CALLBACK (caps_set), session);
  } else if (strcmp (e->name, TSMF_DVC_CHANNEL_NAME) == 0) {
    // TODO Old windows 7 multimedia redirection
  } else if (strcmp (e->name, RDPGFX_DVC_CHANNEL_NAME) == 0) {
    gdi_graphics_pipeline_init (ctx->context.gdi, (RdpgfxClientContext *) e->pInterface);
  } else if (strcmp (e->name, RAIL_SVC_CHANNEL_NAME) == 0) {
    // TODO Remote application
  } else if (strcmp (e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0) {
#ifdef FRDP_WITH_CLIPBOARD
    g_clear_object (&priv->clipboard_channel);

    priv->clipboard_channel = g_object_new (FRDP_TYPE_CHANNEL_CLIPBOARD,
                                            "session", session,
                                            "cliprdr-client-context", (CliprdrClientContext *) e->pInterface,
                                            NULL);
#endif
  } else if (strcmp (e->name, ENCOMSP_SVC_CHANNEL_NAME) == 0) {
    // TODO Multiparty channel
  } else if (strcmp (e->name, GEOMETRY_DVC_CHANNEL_NAME) == 0) {
    gdi_video_geometry_init (ctx->context.gdi, (GeometryClientContext *) e->pInterface);
  } else if (strcmp (e->name, VIDEO_CONTROL_DVC_CHANNEL_NAME) == 0) {
    gdi_video_control_init (ctx->context.gdi, (VideoClientContext *) e->pInterface);
  } else if (strcmp (e->name, VIDEO_DATA_DVC_CHANNEL_NAME) == 0) {
    gdi_video_data_init (ctx->context.gdi, (VideoClientContext *) e->pInterface);
  }
}

static void
frdp_on_channel_disconnected_event_handler (void                                         *context,
                                            CONST_QUALIFIER ChannelDisconnectedEventArgs *e)
{
  frdpContext        *ctx = (frdpContext *) context;
  FrdpSession        *session = ctx->self;
  FrdpSessionPrivate *priv = frdp_session_get_instance_private (session);

  if (strcmp (e->name, RDPEI_DVC_CHANNEL_NAME) == 0) {
    // TODO Touch input redirection
  } else if (strcmp (e->name, DISP_DVC_CHANNEL_NAME) == 0) {
    g_clear_object (&priv->display_control_channel);
  } else if (strcmp (e->name, TSMF_DVC_CHANNEL_NAME) == 0) {
    // TODO Old windows 7 multimedia redirection
  } else if (strcmp (e->name, RDPGFX_DVC_CHANNEL_NAME) == 0) {
    gdi_graphics_pipeline_uninit (ctx->context.gdi, (RdpgfxClientContext *) e->pInterface);
  } else if (strcmp (e->name, RAIL_SVC_CHANNEL_NAME) == 0) {
    // TODO Remote application
  } else if (strcmp (e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0) {
#ifdef FRDP_WITH_CLIPBOARD
    g_clear_object (&priv->clipboard_channel);
#endif
  } else if (strcmp (e->name, ENCOMSP_SVC_CHANNEL_NAME) == 0) {
    // TODO Multiparty channel
  } else if (strcmp (e->name, GEOMETRY_DVC_CHANNEL_NAME) == 0) {
    gdi_video_geometry_uninit (ctx->context.gdi, (GeometryClientContext *) e->pInterface);
  } else if (strcmp (e->name, VIDEO_CONTROL_DVC_CHANNEL_NAME) == 0) {
    gdi_video_control_uninit (ctx->context.gdi, (VideoClientContext *) e->pInterface);
  } else if (strcmp (e->name, VIDEO_DATA_DVC_CHANNEL_NAME) == 0) {
    gdi_video_data_uninit (ctx->context.gdi, (VideoClientContext *) e->pInterface);
  }
}

static gboolean
frdp_load_channels (freerdp *instance)
{
  if (!freerdp_client_load_addins (instance->context->channels, instance->context->settings))
    return FALSE;

  return TRUE;
}

static gboolean
frdp_pre_connect (freerdp *freerdp_session)
{
  rdpSettings *settings = freerdp_session->context->settings;
  rdpContext *context = freerdp_session->context;
  BYTE *OrderSupport = freerdp_settings_get_pointer_writable(settings, FreeRDP_OrderSupport);

  if (OrderSupport != NULL) {
    OrderSupport[NEG_DSTBLT_INDEX] = TRUE;
    OrderSupport[NEG_PATBLT_INDEX] = TRUE;
    OrderSupport[NEG_SCRBLT_INDEX] = TRUE;
    OrderSupport[NEG_OPAQUE_RECT_INDEX] = TRUE;
    OrderSupport[NEG_DRAWNINEGRID_INDEX] = FALSE;
    OrderSupport[NEG_MULTIDSTBLT_INDEX] = FALSE;
    OrderSupport[NEG_MULTIPATBLT_INDEX] = FALSE;
    OrderSupport[NEG_MULTISCRBLT_INDEX] = FALSE;
    OrderSupport[NEG_MULTIOPAQUERECT_INDEX] = TRUE;
    OrderSupport[NEG_MULTI_DRAWNINEGRID_INDEX] = FALSE;
    OrderSupport[NEG_LINETO_INDEX] = TRUE;
    OrderSupport[NEG_POLYLINE_INDEX] = TRUE;
    OrderSupport[NEG_MEMBLT_INDEX] = TRUE;
    OrderSupport[NEG_MEM3BLT_INDEX] = FALSE;
    OrderSupport[NEG_MEMBLT_V2_INDEX] = TRUE;
    OrderSupport[NEG_MEM3BLT_V2_INDEX] = FALSE;
    OrderSupport[NEG_SAVEBITMAP_INDEX] = FALSE;
    OrderSupport[NEG_GLYPH_INDEX_INDEX] = TRUE;
    OrderSupport[NEG_FAST_INDEX_INDEX] = TRUE;
    OrderSupport[NEG_FAST_GLYPH_INDEX] = FALSE;
    OrderSupport[NEG_POLYGON_SC_INDEX] = FALSE;
    OrderSupport[NEG_POLYGON_CB_INDEX] = FALSE;
    OrderSupport[NEG_ELLIPSE_SC_INDEX] = FALSE;
    OrderSupport[NEG_ELLIPSE_CB_INDEX] = FALSE;
  }

  PubSub_SubscribeChannelConnected (context->pubSub,
                                    frdp_on_channel_connected_event_handler);
  PubSub_SubscribeChannelDisconnected (context->pubSub,
                                       frdp_on_channel_disconnected_event_handler);

#ifndef HAVE_FREERDP3
  return frdp_load_channels (freerdp_session);
#else
  return TRUE;
#endif
}

static gboolean
frdp_begin_paint (rdpContext *context)
{
  rdpGdi *gdi = context->gdi;

  gdi->primary->hdc->hwnd->invalid->null = 1;
  gdi->primary->hdc->hwnd->ninvalid = 0;

  return TRUE;
}

/* GTK4 has no partial invalidation; FreeRDP's GDI already tracks the dirty
 * region, so a flag that makes the next snapshot re-upload is enough. */
static gboolean
frdp_end_paint (rdpContext *context)
{
  FrdpSession *self = ((frdpContext *) context)->self;
  rdpGdi *gdi = context->gdi;

  if (gdi->primary->hdc->hwnd->invalid->null)
    return TRUE;

  self->priv->frame_dirty = TRUE;

  return TRUE;
}

static gboolean
frdp_post_connect (freerdp *freerdp_session)
{
  rdpSettings *settings;
  rdpContext *context;
  FrdpSession *self = ((frdpContext *) freerdp_session->context)->self;
  ResizeWindowEventArgs e;

  context = freerdp_session->context;
  settings = context->settings;

  /* GTK4 has no visuals to probe; GdkMemoryTexture takes 32-bit BGRX. */
  gdi_init (freerdp_session, PIXEL_FORMAT_BGRX32);

  frdp_register_pointer (freerdp_session->context->graphics);
  freerdp_session->context->update->BeginPaint = frdp_begin_paint;
  freerdp_session->context->update->EndPaint = frdp_end_paint;
  freerdp_session->context->update->DesktopResize = frdp_desktop_resize;

  EventArgsInit(&e, "frdp");
	e.width = freerdp_settings_get_uint32 (settings, FreeRDP_DesktopWidth);
	e.height = freerdp_settings_get_uint32 (settings, FreeRDP_DesktopHeight);
	PubSub_OnResizeWindow(context->pubSub, freerdp_session->context, &e);

  frdp_session_update_size_request (self);
  frdp_session_update_scale (self);
  self->priv->frame_dirty = TRUE;

  return TRUE;
}

static void
frdp_post_disconnect (freerdp *instance)
{
  FrdpSession *self;
  rdpContext  *context;

  if (!instance || !instance->context)
    return;

  self = ((frdpContext *) instance->context)->self;

  if (self->priv->display != NULL)
    g_signal_handlers_disconnect_by_func (self->priv->display, G_CALLBACK (frdp_session_resize_supported_changed), self);

  context = instance->context;
  PubSub_UnsubscribeChannelConnected (context->pubSub,
                                      frdp_on_channel_connected_event_handler);
  PubSub_UnsubscribeChannelDisconnected (context->pubSub,
                                         frdp_on_channel_disconnected_event_handler);

  frdp_pointer_set_null (context);

  gdi_free (instance);
}

static gboolean
idle_close (gpointer user_data)
{
  FrdpSession *self = (FrdpSession*) user_data;

  gboolean was_open = self->priv->is_connected || self->priv->freerdp_session != NULL;

  if (self->priv->close_idle_id != 0) {
    guint id = self->priv->close_idle_id;

    /* Removing it drops the idle's reference; we are not the last holder
     * (finalize cannot run while the idle still owns one). */
    self->priv->close_idle_id = 0;
    g_source_remove (id);
  }

  self->priv->is_connected = FALSE;

  if (self->priv->update_id > 0) {
    g_source_remove (self->priv->update_id);
    self->priv->update_id = 0;
  }

  if (self->priv->resize_timeout_id > 0) {
    g_source_remove (self->priv->resize_timeout_id);
    self->priv->resize_timeout_id = 0;
  }

  if (self->priv->freerdp_session != NULL) {
    freerdp_disconnect (self->priv->freerdp_session);
    /* freerdp_free() only frees the instance struct (verified in FreeRDP
     * 3.32.1 libfreerdp/core/freerdp.c); upstream never freed the context,
     * leaking settings, channels and the GDI framebuffer per session. */
    if (self->priv->freerdp_session->context != NULL)
      freerdp_context_free (self->priv->freerdp_session);
    g_clear_pointer (&self->priv->freerdp_session, freerdp_free);
  }

  g_clear_object (&self->priv->frame);
  self->priv->frame_dirty = FALSE;
  self->priv->cursor = NULL;

  if (self->priv->display != NULL)
    gtk_widget_queue_draw (self->priv->display);

  /* Idempotent: dispose, an explicit close and a server disconnect can all
   * land here; only the first one is a state change worth announcing. */
  if (!was_open)
    return FALSE;

  g_signal_emit (self, signals[RDP_DISCONNECTED], 0);
  g_debug ("RDP client disconnected");

  return FALSE;
}

static gboolean
idle_close_cb (gpointer user_data)
{
  FrdpSession *self = user_data;

  self->priv->close_idle_id = 0;
  idle_close (self);

  return G_SOURCE_REMOVE;
}

/* Upstream queued idle_close() with a bare pointer. Once the widget (and
 * with it the session) is actually freed, the queued close ran on freed
 * memory: Gtk-CRITICAL + free(): invalid pointer after a failed connect.
 * The idle now owns a reference, and only one close is ever pending. */
static void
frdp_session_schedule_close (FrdpSession *self)
{
  if (self->priv->close_idle_id != 0)
    return;

  self->priv->close_idle_id = g_idle_add_full (G_PRIORITY_DEFAULT_IDLE,
                                               idle_close_cb,
                                               g_object_ref (self),
                                               g_object_unref);
}

/*
 * FreeRDP event source.
 *
 * Upstream ran an idle callback that blocked the GTK thread in
 * WaitForMultipleObjects() for up to 20 ms per iteration: ~50 wake-ups/s
 * and ~7% of a core while the remote desktop was completely idle, plus up
 * to 20 ms of input latency. This GSource instead hands FreeRDP's handles
 * to the GLib main loop, which sleeps until one is ready:
 *  - Unix/macOS: winpr_Handle_getFd() gives a pollable fd for each handle
 *  - Windows: g_poll() waits on HANDLEs directly (GPollFD.fd is a HANDLE)
 * Handles that have no fd fall back to a short poll timeout.
 */
#define FRDP_MAX_HANDLES 64
#define FRDP_FALLBACK_POLL_MS 20

typedef struct
{
  GSource      source;
  FrdpSession *self;
  GPollFD      fds[FRDP_MAX_HANDLES];
  guint        n_fds;
  gboolean     needs_fallback_poll;
} FrdpEventSource;

/* Re-registers poll fds only when FreeRDP's handle set actually changed.
 * Changing a source's polls makes GLib discard the current iteration and
 * start over without sleeping, so touching them on every prepare() spins
 * a full core (measured). */
static void
frdp_event_source_refresh (FrdpEventSource *src)
{
  FrdpSessionPrivate *priv = src->self->priv;
  HANDLE              handles[FRDP_MAX_HANDLES];
  gintptr             fds[FRDP_MAX_HANDLES];
  guint               n_new = 0;
  gboolean            fallback = FALSE;
  DWORD               n = 0, i;

  if (priv->freerdp_session != NULL)
    n = freerdp_get_event_handles (priv->freerdp_session->context, handles, ARRAYSIZE (handles));
  if (n == 0)
    fallback = TRUE;

  for (i = 0; i < n; i++) {
#ifdef G_OS_WIN32
    gintptr fd = (gintptr) handles[i];
#else
    gintptr fd = GetEventFileDescriptor (handles[i]);
#endif
    if (fd < 0)
      fallback = TRUE;
    else
      fds[n_new++] = fd;
  }

  src->needs_fallback_poll = fallback;

  if (n_new == src->n_fds) {
    gboolean same = TRUE;

    for (i = 0; i < n_new && same; i++)
      same = (src->fds[i].fd == fds[i]);
    if (same)
      return;
  }

  for (i = 0; i < src->n_fds; i++)
    g_source_remove_poll (&src->source, &src->fds[i]);

  for (i = 0; i < n_new; i++) {
    src->fds[i].fd = fds[i];
    src->fds[i].events = G_IO_IN | G_IO_HUP | G_IO_ERR;
    src->fds[i].revents = 0;
    g_source_add_poll (&src->source, &src->fds[i]);
  }
  src->n_fds = n_new;
}

static gboolean
frdp_event_source_prepare (GSource *source, gint *timeout)
{
  FrdpEventSource *src = (FrdpEventSource *) source;

  /* The handle set changes as channels come and go; re-read it each cycle.
   * It is a handful of handles, so this is cheap. */
  frdp_event_source_refresh (src);
  *timeout = src->needs_fallback_poll ? FRDP_FALLBACK_POLL_MS : -1;

  /* Never "ready" just because a frame is dirty: frames only become dirty
   * inside dispatch (FreeRDP paints during freerdp_check_event_handles),
   * which queues the draw itself, and GTK snapshots on map anyway.
   * Reporting dirty here would spin until the next frame clock tick. */
  return FALSE;
}

static gboolean
frdp_event_source_check (GSource *source)
{
  FrdpEventSource *src = (FrdpEventSource *) source;
  guint            i;

  if (src->needs_fallback_poll)
    return TRUE;

  for (i = 0; i < src->n_fds; i++)
    if (src->fds[i].revents != 0)
      return TRUE;

  return FALSE;
}

static gboolean
frdp_event_source_dispatch (GSource *source, GSourceFunc callback, gpointer user_data)
{
  FrdpEventSource    *src = (FrdpEventSource *) source;
  FrdpSession        *self = src->self;
  FrdpSessionPrivate *priv = self->priv;

  spike_stats.loops++;
  spike_stats_tick ();

  if (priv->freerdp_session == NULL)
    return G_SOURCE_REMOVE;

  if (!freerdp_check_event_handles (priv->freerdp_session->context) &&
      freerdp_get_last_error (priv->freerdp_session->context) == FREERDP_ERROR_SUCCESS)
    g_warning ("Failed to check FreeRDP event handles");

  if (freerdp_shall_disconnect_context (priv->freerdp_session->context)) {
    priv->update_id = 0;
    frdp_session_schedule_close (self);
    return G_SOURCE_REMOVE;
  }

  /* Upstream skipped all processing while the widget was unmapped, which
   * would stall the connection of a hidden (background) tab. Only drawing
   * is skipped now; the frame stays dirty until the widget is shown. */
  if (priv->frame_dirty && priv->display != NULL && gtk_widget_get_mapped (priv->display)) {
    spike_stats.signaled++;
    gtk_widget_queue_draw (priv->display);
  }

  return G_SOURCE_CONTINUE;
}

static GSourceFuncs frdp_event_source_funcs = {
  frdp_event_source_prepare,
  frdp_event_source_check,
  frdp_event_source_dispatch,
  NULL, NULL, NULL
};

static guint
frdp_session_attach_event_source (FrdpSession *self)
{
  GSource         *source = g_source_new (&frdp_event_source_funcs, sizeof (FrdpEventSource));
  FrdpEventSource *src = (FrdpEventSource *) source;
  guint            id;

  src->self = self;
  src->n_fds = 0;
  g_source_set_name (source, "FreeRDP events");
  id = g_source_attach (source, NULL);
  g_source_unref (source);

  return id;
}

/*
 *  The local layout string is taken from org.gnome.desktop.input-sources.mru-sources
 *  after adding it among available sources and setting it active.
 *  Most of the national layout has been assigned according to default keyboard
 *  for given nation so these can be inaccurate.
 *  Non-standard layouts has to be checked directly by comparing them
 *  (e.g. against layouts on https://kbdlayout.info/).
 */
static const struct {
  const char  *local_layout;
  const guint  freerdp_layout;
} keyboard_layouts[] = {
  { "ara", KBD_ARABIC_101 },
  { "bg", KBD_BULGARIAN },
  { "cz", KBD_CZECH },
  { "da", KBD_DANISH },
  { "de", KBD_GERMAN },
  { "gr", KBD_GREEK },
  { "us", KBD_US },
  { "es", KBD_SPANISH },
  { "fi", KBD_FINNISH },
  { "fr", KBD_FRENCH },
  { "il", KBD_HEBREW },
  { "hu", KBD_HUNGARIAN },
  { "is", KBD_ICELANDIC },
  { "it", KBD_ITALIAN },
  { "jp", KBD_JAPANESE },
  { "kr", KBD_KOREAN },
  { "nl", KBD_DUTCH },
  { "no", KBD_NORWEGIAN },
  { "pl+dvp", KBD_POLISH_PROGRAMMERS },
  { "pt", KBD_PORTUGUESE_BRAZILIAN_ABNT },
  { "ro", KBD_ROMANIAN },
  { "ru", KBD_RUSSIAN },
  { "hr", KBD_CROATIAN },
  { "sk", KBD_SLOVAK },
  { "al", KBD_ALBANIAN },
  { "se", KBD_SWEDISH },
  { "tr", KBD_TURKISH_Q },
  { "pk+urd-nla", KBD_URDU },
  { "ua", KBD_UKRAINIAN },
  { "by", KBD_BELARUSIAN },
  { "si", KBD_SLOVENIAN },
  { "ee", KBD_ESTONIAN },
  { "lv", KBD_LATVIAN },
  { "lt+ibm", KBD_LITHUANIAN_IBM },
  { "vn", KBD_VIETNAMESE },
  { "am+eastern", KBD_ARMENIAN_EASTERN },
  { "mk", KBD_FYRO_MACEDONIAN },
  { "ge", KBD_GEORGIAN },
  { "fo", KBD_FAEROESE },
  { "no+smi", KBD_NORWEGIAN_WITH_SAMI },
  { "kz", KBD_KAZAKH },
  { "gb+extd", KBD_UNITED_KINGDOM_EXTENDED },
  { "sy+syc", KBD_SYRIAC },
  { "af+ps-olpc", KBD_PASHTO },
  { "m17n:dv:phonetic", KBD_DIVEHI_PHONETIC },
  { "nz+mao", KBD_MAORI },
  { "ch", KBD_SWISS_GERMAN },
  { "gb", KBD_UNITED_KINGDOM },
  { "latam", KBD_LATIN_AMERICAN },
  { "be", KBD_BELGIAN_FRENCH },
  { "pt", KBD_PORTUGUESE },
  { "ca+fr-legacy", KBD_CANADIAN_FRENCH_LEGACY },
  { "ca", KBD_CANADIAN_FRENCH },
  { "ba", KBD_BOSNIAN },
  { "cz+qwerty", KBD_CZECH_QWERTY },
  { "de+T3", KBD_GERMAN_IBM },
  { "us+dvorak", KBD_UNITED_STATES_DVORAK },
  { "it+ibm", KBD_ITALIAN_142 },
  { "pl+qwertz", KBD_POLISH_214 },
  { "pt", KBD_PORTUGUESE_BRAZILIAN_ABNT2 },
  { "sk+qwerty", KBD_SLOVAK_QWERTY },
  { "th+pat", KBD_THAI_PATTACHOTE },
  { "tr+f", KBD_TURKISH_F },
  { "lt", KBD_LITHUANIAN },
  { "am+western", KBD_ARMENIAN_WESTERN },
  { "be+iso-alternate", KBD_BELGIAN_COMMA },
  { "gb+gla", KBD_GAELIC },
  { "us+intl", KBD_UNITED_STATES_INTERNATIONAL },
  { "us+dvorak-l", KBD_UNITED_STATES_DVORAK_FOR_LEFT_HAND },
  { "us+dvorak-r", KBD_UNITED_STATES_DVORAK_FOR_RIGHT_HAND },
  { "us+dvp", KBD_UNITED_STATES_DVORAK_PROGRAMMER },
  { "gr+polytonic", KBD_GREEK_POLYTONIC },
  { "fr+bepo", KBD_FRENCH_BEPO },
  { "de+neo", KBD_GERMAN_NEO },

  /* These need to be determined yet. */

  { "", KBD_CHINESE_TRADITIONAL_US },
  { "", KBD_THAI_KEDMANEE },
  { "", KBD_FARSI },
  { "", KBD_AZERI_LATIN },
  { "", KBD_DEVANAGARI_INSCRIPT },
  { "", KBD_MALTESE_47_KEY },
  { "", KBD_KYRGYZ_CYRILLIC },
  { "", KBD_TATAR },
  { "", KBD_BENGALI },
  { "", KBD_PUNJABI },
  { "", KBD_GUJARATI },
  { "", KBD_TAMIL },
  { "", KBD_TELUGU },
  { "", KBD_KANNADA },
  { "", KBD_MALAYALAM },
  { "", KBD_MARATHI },
  { "", KBD_MONGOLIAN_CYRILLIC },
  { "", KBD_NEPALI },
  { "", KBD_LUXEMBOURGISH },
  { "", KBD_CHINESE_SIMPLIFIED_US },
  { "", KBD_BELGIAN_PERIOD },
  { "", KBD_SERBIAN_LATIN },
  { "", KBD_AZERI_CYRILLIC },
  { "", KBD_SWEDISH_WITH_SAMI },
  { "", KBD_UZBEK_CYRILLIC },
  { "", KBD_INUKTITUT_LATIN },
  { "", KBD_SERBIAN_CYRILLIC },
  { "", KBD_SWISS_FRENCH },
  { "", KBD_IRISH },
  { "", KBD_BOSNIAN_CYRILLIC },
  { "", KBD_ARABIC_102 },
  { "", KBD_BULGARIAN_LATIN },
  { "", KBD_GREEK_220 },
  { "", KBD_SPANISH_VARIATION },
  { "", KBD_HUNGARIAN_101_KEY },
  { "", KBD_RUSSIAN_TYPEWRITER },
  { "", KBD_LATVIAN_QWERTY },
  { "", KBD_HINDI_TRADITIONAL },
  { "", KBD_MALTESE_48_KEY },
  { "", KBD_SAMI_EXTENDED_NORWAY },
  { "", KBD_BENGALI_INSCRIPT },
  { "", KBD_SYRIAC_PHONETIC },
  { "", KBD_DIVEHI_TYPEWRITER },
  { "", KBD_FINNISH_WITH_SAMI },
  { "", KBD_CANADIAN_MULTILINGUAL_STANDARD },
  { "", KBD_ARABIC_102_AZERTY },
  { "", KBD_CZECH_PROGRAMMERS },
  { "", KBD_GREEK_319 },
  { "", KBD_THAI_KEDMANEE_NON_SHIFTLOCK },
  { "", KBD_SAMI_EXTENDED_FINLAND_SWEDEN },
  { "", KBD_GREEK_220_LATIN },
  { "", KBD_THAI_PATTACHOTE_NON_SHIFTLOCK },
  { "", KBD_GREEK_319_LATIN },
  { "", KBD_GREEK_LATIN },
  { "", KBD_US_ENGLISH_TABLE_FOR_IBM_ARABIC_238_L }

/*
  These are available in FreeRDP 3:
  { "", KBD_ROMANIAN_STANDARD },
  { "", KBD_GEORGIAN_QUERTY },
  { "", KBD_KHMER },
  { "", KBD_RUSSIAN_PHONETIC },
  { "", KBD_BANGLA },
  { "", KBD_BULGARIAN_PHONETIC },
  { "", KBD_PERSIAN },
*/
};

static void
frdp_session_set_current_keyboard_layout (FrdpSession *self) {
  GSettingsSchemaSource *source;
  FrdpSessionPrivate    *priv = self->priv;
  GSettingsSchema       *schema;
  rdpSettings           *settings;
  const gchar           *source_type = NULL, *layout = NULL;
  GSettings             *gsettings;
  GVariant              *sources;
  gboolean               keyboard_layout_set = FALSE;
  guint                  i;

  settings = priv->freerdp_session->context->settings;

  source = g_settings_schema_source_get_default ();
  if (source != NULL) {
    schema = g_settings_schema_source_lookup (source, "org.gnome.desktop.input-sources", TRUE);
    if (schema != NULL) {
      gsettings = g_settings_new (g_settings_schema_get_id (schema));
      sources = g_settings_get_value (gsettings, "mru-sources");

      if (g_variant_n_children (sources) >= 1)
        g_variant_get_child (sources, 0, "(&s&s)", &source_type, &layout);

      if (layout != NULL) {
        for (i = 0; i < G_N_ELEMENTS (keyboard_layouts); i++) {
          if (g_strcmp0 (layout, keyboard_layouts[i].local_layout) == 0) {
#ifdef HAVE_FREERDP_3_11_0
            freerdp_settings_set_uint32 (settings, FreeRDP_KeyboardLayout, keyboard_layouts[i].freerdp_layout);
#else
            settings->KeyboardLayout = freerdp_keyboard_init (keyboard_layouts[i].freerdp_layout);
#endif
            keyboard_layout_set = TRUE;
            break;
          }
        }
      }

      g_variant_unref (sources);
      g_object_unref (gsettings);
      g_settings_schema_unref (schema);
    }
  }

#ifndef HAVE_FREERDP_3_11_0
  if (!keyboard_layout_set)
    settings->KeyboardLayout = freerdp_keyboard_init (0);
#endif
}

static gboolean
frdp_session_init_freerdp (FrdpSession *self)
{
  CONST_QUALIFIER gchar *collections[] = { "disp" };
  FrdpSessionPrivate    *priv = self->priv;
  rdpSettings           *settings;
  gchar                 *build_options;
  int                    count = 1;

  /* Setup FreeRDP session */
  priv->freerdp_session = freerdp_new ();

  if (priv->freerdp_session == NULL)
    return FALSE;

  priv->freerdp_session->PreConnect = frdp_pre_connect;
  priv->freerdp_session->PostConnect = frdp_post_connect;
  priv->freerdp_session->PostDisconnect = frdp_post_disconnect;
  priv->freerdp_session->AuthenticateEx = frdp_authenticate_ex;
  priv->freerdp_session->VerifyCertificateEx = frdp_certificate_verify_ex;
  priv->freerdp_session->VerifyChangedCertificateEx = frdp_changed_certificate_verify_ex;
#ifdef HAVE_FREERDP3
  priv->freerdp_session->LoadChannels = frdp_load_channels;
#endif

  priv->freerdp_session->ContextSize = sizeof (frdpContext);

  if (!freerdp_context_new (priv->freerdp_session))
    return FALSE;

  ((frdpContext *) priv->freerdp_session->context)->self = self;

  settings = priv->freerdp_session->context->settings;

  freerdp_settings_set_string (settings, FreeRDP_ServerHostname, priv->hostname);
  freerdp_settings_set_uint32 (settings, FreeRDP_ServerPort, priv->port);
  freerdp_settings_set_string (settings, FreeRDP_Username, priv->username);
  freerdp_settings_set_string (settings, FreeRDP_Password, priv->password);
  freerdp_settings_set_string (settings, FreeRDP_Domain, priv->domain);

  freerdp_settings_set_bool (settings, FreeRDP_AllowFontSmoothing, TRUE);
  freerdp_settings_set_bool (settings, FreeRDP_AllowUnanouncedOrdersFromServer, TRUE);

  /* Security settings */
  freerdp_settings_set_bool (settings, FreeRDP_RdpSecurity, TRUE);
  freerdp_settings_set_bool (settings, FreeRDP_TlsSecurity, TRUE);
  freerdp_settings_set_bool (settings, FreeRDP_NlaSecurity, TRUE);
  freerdp_settings_set_uint32 (settings, FreeRDP_EncryptionMethods, ENCRYPTION_METHOD_40BIT | ENCRYPTION_METHOD_128BIT | ENCRYPTION_METHOD_FIPS);
  freerdp_settings_set_uint32 (settings, FreeRDP_EncryptionLevel, ENCRYPTION_LEVEL_CLIENT_COMPATIBLE);
  freerdp_settings_set_bool (settings, FreeRDP_UseRdpSecurityLayer, FALSE);

  freerdp_settings_set_bool (settings, FreeRDP_NegotiateSecurityLayer, TRUE);

  freerdp_settings_set_bool (settings, FreeRDP_DesktopResize, TRUE);
  freerdp_settings_set_bool (settings, FreeRDP_DynamicResolutionUpdate, TRUE);
  freerdp_settings_set_bool (settings, FreeRDP_SupportDisplayControl, TRUE);
  freerdp_settings_set_bool (settings, FreeRDP_RemoteFxCodec, TRUE);
  freerdp_settings_set_uint32 (settings, FreeRDP_ColorDepth, 32);
  freerdp_settings_set_bool (settings, FreeRDP_RedirectClipboard, TRUE);
  freerdp_settings_set_bool (settings, FreeRDP_SupportGraphicsPipeline, TRUE);

  freerdp_client_add_dynamic_channel (settings, count, collections);

  build_options = g_ascii_strup (freerdp_get_build_config (), -1);
  if (g_strrstr (build_options, "WITH_GFX_H264=ON") != NULL) {
    freerdp_settings_set_bool (settings, FreeRDP_GfxH264, TRUE);
    freerdp_settings_set_bool (settings, FreeRDP_GfxAVC444, TRUE);
  } else {
    freerdp_settings_set_bool (settings, FreeRDP_GfxH264, FALSE);
    freerdp_settings_set_bool (settings, FreeRDP_GfxAVC444, FALSE);
  }
  g_free (build_options);

  frdp_session_set_current_keyboard_layout (self);

  freerdp_register_addin_provider(freerdp_channels_load_static_addin_entry, 0);

  return TRUE;
}

static void
frdp_session_connect_thread (GTask        *task,
                             gpointer      source_object,
                             gpointer      task_data,
                             GCancellable *cancellable)
{
  FrdpSession *self = (FrdpSession*) source_object;
  guint32      error_code;

  if (!frdp_session_init_freerdp (self)) {
    if (self->priv->freerdp_session != NULL &&
        self->priv->freerdp_session->context != NULL) {
      error_code = freerdp_get_last_error (self->priv->freerdp_session->context);
      g_signal_emit (self,
                     signals[RDP_ERROR], 0,
                     freerdp_get_last_error_string (error_code));
      g_warning ("Failed to initialize RDP with error '%s'",
                 freerdp_get_last_error_string (error_code));
    } else {
      g_signal_emit (self,
                     signals[RDP_ERROR], 0,
                     "Failed to initialize RDP!");
    }

    frdp_session_schedule_close (self);
    g_task_return_boolean (task, FALSE);

    return;
  }

  self->priv->is_connected = freerdp_connect (self->priv->freerdp_session);
  if (!self->priv->is_connected) {
    error_code = freerdp_get_last_error (self->priv->freerdp_session->context);
    switch (error_code) {
        case FREERDP_ERROR_AUTHENTICATION_FAILED:
        case FREERDP_ERROR_CONNECT_FAILED:
        case FREERDP_ERROR_SERVER_DENIED_CONNECTION:
        case FREERDP_ERROR_CONNECT_NO_OR_MISSING_CREDENTIALS:
        case FREERDP_ERROR_CONNECT_LOGON_FAILURE:
        case STATUS_LOGON_FAILURE:
        case STATUS_PASSWORD_EXPIRED:
        case FREERDP_ERROR_CONNECT_ACCOUNT_EXPIRED:
        case FREERDP_ERROR_CONNECT_TRANSPORT_FAILED:
        case ERRCONNECT_CONNECT_TRANSPORT_FAILED:
        case FREERDP_ERROR_TLS_CONNECT_FAILED:
        case FREERDP_ERROR_DNS_NAME_NOT_FOUND:
            g_signal_emit (self,
                           signals[RDP_AUTH_FAILURE], 0,
                           freerdp_get_last_error_string (error_code));

            g_warning ("Failed to connect RDP host with error '%s'",
                       freerdp_get_last_error_string (error_code));
            break;

        default:
            g_signal_emit (self,
                           signals[RDP_ERROR], 0,
                           freerdp_get_last_error_string (error_code));

            g_warning ("Unexpected RDP error: '%s'",
                       freerdp_get_last_error_string (error_code));
            break;
    }

    frdp_session_schedule_close (self);
    g_task_return_boolean (task, FALSE);

    return;
  }

  frdp_session_update_size_request (self);
  frdp_session_update_scale (self);
  self->priv->frame_dirty = TRUE;
  g_signal_connect (self->priv->display, "notify::resize-supported",
                    G_CALLBACK (frdp_session_resize_supported_changed), self);

  self->priv->update_id = frdp_session_attach_event_source (self);

  g_task_return_boolean (task, TRUE);
}

static void
frdp_session_get_property (GObject    *object,
                           guint       property_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  FrdpSession *self = (FrdpSession*) object;

  switch (property_id)
    {
      case PROP_HOSTNAME:
        g_value_set_string (value, self->priv->hostname);
        break;
      case PROP_PORT:
        g_value_set_uint (value, self->priv->port);
        break;
      case PROP_USERNAME:
        g_value_set_string (value, self->priv->username);
        break;
      case PROP_PASSWORD:
        g_value_set_string (value, self->priv->password);
        break;
      case PROP_DOMAIN:
        g_value_set_string (value, self->priv->domain);
        break;
      case PROP_DISPLAY:
        g_value_set_object (value, self->priv->display);
        break;
      case PROP_SCALING:
        g_value_set_boolean (value, self->priv->scaling);
        break;
      case PROP_MONITOR_LAYOUT_SUPPORTED:
        g_value_set_boolean (value, self->priv->monitor_layout_supported);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
frdp_session_set_property (GObject      *object,
                           guint         property_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  FrdpSession *self = (FrdpSession*) object;

  switch (property_id)
    {
      case PROP_HOSTNAME:
        g_free (self->priv->hostname);
        self->priv->hostname = g_value_dup_string (value);
        break;
      case PROP_PORT:
        self->priv->port = g_value_get_uint (value);
        break;
      case PROP_USERNAME:
        g_free (self->priv->username);
        self->priv->username = g_value_dup_string (value);
        break;
      case PROP_PASSWORD:
        g_free (self->priv->password);
        self->priv->password = g_value_dup_string (value);
        break;
      case PROP_DOMAIN:
        g_free (self->priv->domain);
        self->priv->domain = g_value_dup_string (value);
        break;
      case PROP_DISPLAY:
        self->priv->display = g_value_get_object (value);
        break;
      case PROP_SCALING:
        frdp_session_set_scaling (self, g_value_get_boolean (value));
        break;
      case PROP_MONITOR_LAYOUT_SUPPORTED:
        self->priv->monitor_layout_supported = g_value_get_boolean (value);
        g_object_notify (G_OBJECT (self), "monitor-layout-supported");
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
frdp_session_finalize (GObject *object)
{
  FrdpSession *self = (FrdpSession*) object;

  idle_close (self);

  g_free (self->priv->hostname);
  g_free (self->priv->username);
  if (self->priv->password != NULL)
    memset (self->priv->password, 0, strlen (self->priv->password));
  g_free (self->priv->password);
  g_free (self->priv->domain);

  G_OBJECT_CLASS (frdp_session_parent_class)->finalize (object);
}

static void
frdp_session_class_init (FrdpSessionClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = frdp_session_finalize;
  gobject_class->get_property = frdp_session_get_property;
  gobject_class->set_property = frdp_session_set_property;

  g_object_class_install_property (gobject_class,
                                   PROP_HOSTNAME,
                                   g_param_spec_string ("hostname",
                                                        "hostname",
                                                        "hostname",
                                                        NULL,
                                                        G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class,
                                   PROP_PORT,
                                   g_param_spec_uint ("port",
                                                      "port",
                                                      "port",
                                                       0, G_MAXUINT16, 3389,
                                                       G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE));

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
                                   PROP_DISPLAY,
                                   g_param_spec_object ("display",
                                                        "display",
                                                        "display",
                                                        GTK_TYPE_WIDGET,
                                                        G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class,
                                   PROP_SCALING,
                                   g_param_spec_boolean ("scaling",
                                                         "scaling",
                                                         "scaling",
                                                         TRUE,
                                                         G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class,
                                   PROP_MONITOR_LAYOUT_SUPPORTED,
                                   g_param_spec_boolean ("monitor-layout-supported",
                                                         "monitor-layout-supported",
                                                         "monitor-layout-supported",
                                                         FALSE,
                                                         G_PARAM_READWRITE));

  signals[RDP_ERROR] = g_signal_new ("rdp-error",
                                     FRDP_TYPE_SESSION,
                                     G_SIGNAL_RUN_FIRST,
                                     0, NULL, NULL, NULL,
                                     G_TYPE_NONE, 1,
                                     G_TYPE_STRING);

  signals[RDP_CONNECTED] = g_signal_new ("rdp-connected",
                                         FRDP_TYPE_SESSION,
                                         G_SIGNAL_RUN_FIRST,
                                         0, NULL, NULL, NULL,
                                         G_TYPE_NONE, 0);

  signals[RDP_DISCONNECTED] = g_signal_new ("rdp-disconnected",
                                            FRDP_TYPE_SESSION,
                                            G_SIGNAL_RUN_FIRST,
                                            0, NULL, NULL, NULL,
                                            G_TYPE_NONE, 0);

  signals[RDP_AUTH_FAILURE] = g_signal_new ("rdp-auth-failure",
                                            FRDP_TYPE_SESSION,
                                            G_SIGNAL_RUN_FIRST,
                                            0, NULL, NULL, NULL,
                                            G_TYPE_NONE, 1,
                                            G_TYPE_STRING);
}

static void
frdp_session_init (FrdpSession *self)
{
  self->priv = frdp_session_get_instance_private (self);

  self->priv->is_connected = FALSE;
  self->priv->scale = 1.0;
}

FrdpSession*
frdp_session_new (FrdpDisplay *display)
{
  return g_object_new (FRDP_TYPE_SESSION,
                       "display", display,
                       NULL);
}

void
frdp_session_connect (FrdpSession         *self,
                      const gchar         *hostname,
                      guint                port,
                      GCancellable        *cancellable,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data)
{
  GTask *task;

  g_free (self->priv->hostname);
  self->priv->hostname = g_strdup (hostname);
  self->priv->port = port;

  task = g_task_new (self, cancellable, callback, user_data);
  /* Turn off the asynchronous connection via GTask thread as the FreeRDP
   * process then runs in the new thread which makes it hard to cooperate
   * with the original thread running UI leading to random race conditions.
   * Turn it on again if there will be an async support for connection
   * added to FreeRDP.
   * The disadvantage is that the application freezes during connection
   * for some time.

     g_task_run_in_thread (task, frdp_session_connect_thread);

   */

  frdp_session_connect_thread (task, self, user_data, cancellable);

  g_object_unref (task);
}

gboolean
frdp_session_connect_finish (FrdpSession   *self,
                             GAsyncResult  *result,
                             GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

gboolean
frdp_session_is_open (FrdpSession *self)
{
  return self->priv->is_connected;
}

void
frdp_session_close (FrdpSession *self)
{
  idle_close (self);

  g_debug ("Closing RDP session");
}

void
frdp_session_mouse_event (FrdpSession          *self,
                          FrdpMouseEvent        event,
                          guint16               x,
                          guint16               y)
{
  FrdpSessionPrivate *priv = self->priv;
  rdpInput *input;
  guint16 flags = 0;
  guint16 xflags = 0;

  g_return_if_fail (priv->freerdp_session != NULL);

  if (event & FRDP_MOUSE_EVENT_MOVE)
    flags |= PTR_FLAGS_MOVE;
  if (event & FRDP_MOUSE_EVENT_DOWN)
    flags |= PTR_FLAGS_DOWN;
  if (event & FRDP_MOUSE_EVENT_WHEEL) {
    flags |= PTR_FLAGS_WHEEL;
    if (event & FRDP_MOUSE_EVENT_WHEEL_NEGATIVE)
      flags |= PTR_FLAGS_WHEEL_NEGATIVE | 0x0088;
    else
      flags |= 0x0078;
  }
  if (event & FRDP_MOUSE_EVENT_HWHEEL) {
    flags |= PTR_FLAGS_HWHEEL;
    if (event & FRDP_MOUSE_EVENT_WHEEL_NEGATIVE)
      flags |= PTR_FLAGS_WHEEL_NEGATIVE | 0x0088;
    else
      flags |= 0x0078;
  }

  if (event & FRDP_MOUSE_EVENT_BUTTON1)
    flags |= PTR_FLAGS_BUTTON1;
  if (event & FRDP_MOUSE_EVENT_BUTTON2)
    flags |= PTR_FLAGS_BUTTON2;
  if (event & FRDP_MOUSE_EVENT_BUTTON3)
    flags |= PTR_FLAGS_BUTTON3;
  if (event & FRDP_MOUSE_EVENT_BUTTON4)
    xflags |=  PTR_XFLAGS_BUTTON1;
  if (event & FRDP_MOUSE_EVENT_BUTTON5)
    xflags |=  PTR_XFLAGS_BUTTON2;

  input = priv->freerdp_session->context->input;

  if (priv->scaling) {
    x = (x - priv->offset_x) / priv->scale;
    y = (y - priv->offset_y) / priv->scale;
  }

  x = x < 0.0 ? 0.0 : x;
  y = y < 0.0 ? 0.0 : y;
  if (xflags != 0) {
    if (event & FRDP_MOUSE_EVENT_DOWN)
        xflags |=  PTR_XFLAGS_DOWN;
    freerdp_input_send_extended_mouse_event(input, xflags, x, y);
  } else if (flags != 0) {
    freerdp_input_send_mouse_event (input, flags, x, y);
  }
}

void
frdp_session_mouse_smooth_scroll_event (FrdpSession          *self,
                                        guint16               x,
                                        guint16               y,
                                        double                delta_x,
                                        double                delta_y)
{
  FrdpSessionPrivate *priv = self->priv;
  rdpInput           *input;
  guint16             flags = 0, value;

  g_return_if_fail (priv->freerdp_session != NULL);

  if (fabs (delta_y) >= fabs (delta_x)) {
    flags |= PTR_FLAGS_WHEEL;
    value = (guint16) round (fabs (delta_y) * 0x78);
    if (value > 0) {
      /* Reversing direction here to reflect the behaviour on local side. */
      if (delta_y < 0.0) {
        if (value > 255)
          value = 255;
        flags |= value & WheelRotationMask;
      } else {
        if (value > 256)
          value = 256;
        flags |= PTR_FLAGS_WHEEL_NEGATIVE;
        flags |= (~value + 1) & WheelRotationMask;
      }
    }
  } else {
    flags |= PTR_FLAGS_HWHEEL;
    value = (guint16) round (fabs (delta_x) * 0x78);
    if (value > 0) {
      if (delta_x < 0.0) {
        if (value > 256)
          value = 256;
        flags |= PTR_FLAGS_WHEEL_NEGATIVE;
        flags |= (~value + 1) & WheelRotationMask;
      } else {
        if (value > 255)
          value = 255;
        flags |= value & WheelRotationMask;
      }
    }
  }

  input = priv->freerdp_session->context->input;

  freerdp_input_send_mouse_event (input, flags, 0, 0);
}

void
frdp_session_mouse_pointer  (FrdpSession          *self,
                             gboolean              enter)
{
  FrdpSessionPrivate *priv = self->priv;

  priv->show_cursor = enter;
  frdp_session_update_mouse_pointer (self);
}

/* GTK4 hands over the platform's hardware keycode. Its meaning differs per
 * GDK backend, so the translation to an RDP scancode is per platform:
 *  - Linux (X11 and Wayland): XKB keycodes
 *  - macOS: Apple virtual keycodes
 *  - Windows: GDK reports the Win32 virtual-key code
 * Only the Linux path has been exercised by the spike. */
void
frdp_session_send_key (FrdpSession  *self,
                       guint         keycode,
                       gboolean      pressed)
{
  rdpInput *input;
  guint32   virtual_code, virtual_scan_code;

  g_return_if_fail (self->priv->freerdp_session != NULL);
  input = self->priv->freerdp_session->context->input;

#if defined(G_OS_WIN32)
  virtual_code = keycode;
#elif defined(__APPLE__)
  virtual_code = GetVirtualKeyCodeFromKeycode (keycode, WINPR_KEYCODE_TYPE_APPLE);
#else
  virtual_code = GetVirtualKeyCodeFromKeycode (keycode, WINPR_KEYCODE_TYPE_XKB);
#endif
  virtual_scan_code = GetVirtualScanCodeFromVirtualKeyCode (virtual_code, WINPR_KBD_TYPE_IBM_ENHANCED);

  if (virtual_scan_code != RDP_SCANCODE_UNKNOWN &&
      (frdp_display_is_keyboard_grabbed (FRDP_DISPLAY (self->priv->display)) ||
       (virtual_scan_code != RDP_SCANCODE_LWIN && virtual_scan_code != RDP_SCANCODE_RWIN)))
    freerdp_input_send_keyboard_event_ex (input, pressed, FALSE, virtual_scan_code);
}
