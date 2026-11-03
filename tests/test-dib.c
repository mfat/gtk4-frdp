/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 mFat
 *
 * CF_DIB <-> GdkTexture conversion used by the clipboard.
 */
#include <string.h>

#include "frdp-dib.h"

/* A 3x2 image: row 0 = red, green, blue; row 1 = white, black, half-red. */
static const guint8 rgba[] = {
  255, 0, 0, 255,   0, 255, 0, 255,   0, 0, 255, 255,
  255, 255, 255, 255, 0, 0, 0, 255,   128, 0, 0, 255,
};

static GdkTexture *
make_texture (void)
{
  g_autoptr(GBytes) bytes = g_bytes_new_static (rgba, sizeof rgba);

  return gdk_memory_texture_new (3, 2, GDK_MEMORY_R8G8B8A8, bytes, 12);
}

static void
assert_pixels (GdkTexture *texture)
{
  GdkTextureDownloader *d = gdk_texture_downloader_new (texture);
  g_autoptr(GBytes) bytes = NULL;
  gsize stride;
  const guint8 *p;
  int y;

  gdk_texture_downloader_set_format (d, GDK_MEMORY_R8G8B8A8);
  bytes = gdk_texture_downloader_download_bytes (d, &stride);
  gdk_texture_downloader_free (d);
  p = g_bytes_get_data (bytes, NULL);

  g_assert_cmpint (gdk_texture_get_width (texture), ==, 3);
  g_assert_cmpint (gdk_texture_get_height (texture), ==, 2);
  for (y = 0; y < 2; y++)
    g_assert_cmpmem (p + y * stride, 12, rgba + y * 12, 12);
}

static void
test_round_trip (void)
{
  g_autoptr(GdkTexture) texture = make_texture ();
  g_autoptr(GBytes) dib = frdp_dib_from_texture (texture);
  g_autoptr(GdkTexture) back = NULL;
  const guint8 *d = g_bytes_get_data (dib, NULL);

  g_assert_cmpuint (g_bytes_get_size (dib), ==, 40 + 3 * 2 * 4);
  g_assert_cmpuint (d[14], ==, 32);                       /* bpp */
  /* bottom-up: the first stored row is the image's last row (white, BGRA) */
  g_assert_cmpmem (d + 40, 4, ((guint8[]) { 255, 255, 255, 255 }), 4);

  back = frdp_dib_to_texture (dib);
  g_assert_nonnull (back);
  assert_pixels (back);
}

static GBytes *
dib_24bpp_top_down (void)
{
  /* 3x2, 24 bpp, top-down (negative height), rows padded to 12 bytes */
  static guint8 dib[40 + 12 * 2] = { 40, 0, 0, 0,  3, 0, 0, 0,  0xfe, 0xff, 0xff, 0xff,
                                     1, 0,  24, 0,  0, 0, 0, 0 };
  static const guint8 rows[] = {
    0, 0, 255,  0, 255, 0,  255, 0, 0,  0, 0, 0,              /* BGR: red, green, blue + pad */
    255, 255, 255,  0, 0, 0,  0, 0, 128,  0, 0, 0,
  };

  memcpy (dib + 40, rows, sizeof rows);
  return g_bytes_new (dib, sizeof dib);
}

static void
test_24bpp_top_down (void)
{
  g_autoptr(GBytes) dib = dib_24bpp_top_down ();
  g_autoptr(GdkTexture) texture = frdp_dib_to_texture (dib);

  g_assert_nonnull (texture);
  assert_pixels (texture);
}

static void
test_v5_bitfields (void)
{
  /* BITMAPV5HEADER, 32 bpp BI_BITFIELDS with an alpha mask, 1x1 */
  guint8 dib[124 + 4] = { 124, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  1, 0,  32, 0,  3, 0, 0, 0 };
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GdkTexture) texture = NULL;

  memcpy (dib + 40, (guint8[]) { 0, 0, 0xff, 0,  0, 0xff, 0, 0,  0xff, 0, 0, 0,  0, 0, 0, 0xff }, 16);
  memcpy (dib + 124, (guint8[]) { 10, 20, 30, 128 }, 4);      /* BGRA */
  bytes = g_bytes_new (dib, sizeof dib);
  texture = frdp_dib_to_texture (bytes);
  g_assert_nonnull (texture);
  g_assert_cmpint (gdk_texture_get_width (texture), ==, 1);
}

static void
test_rejects_bad_input (void)
{
  guint8 short_dib[20] = { 40 };
  guint8 huge[40] = { 40, 0, 0, 0,  0xff, 0xff, 0xff, 0x7f,  0xff, 0xff, 0xff, 0x7f,  1, 0,  32, 0 };
  guint8 indexed[40 + 4] = { 40, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  1, 0,  8, 0 };
  g_autoptr(GBytes) a = g_bytes_new (short_dib, sizeof short_dib);
  g_autoptr(GBytes) b = g_bytes_new (huge, sizeof huge);
  g_autoptr(GBytes) c = g_bytes_new (indexed, sizeof indexed);

  g_assert_null (frdp_dib_to_texture (a));   /* truncated header */
  g_assert_null (frdp_dib_to_texture (b));   /* claims far more pixels than present */
  g_assert_null (frdp_dib_to_texture (c));   /* 8 bpp palette: unsupported */
}

static void
test_bmp_file (void)
{
  g_autoptr(GdkTexture) texture = make_texture ();
  g_autoptr(GBytes) dib = frdp_dib_from_texture (texture);
  g_autoptr(GBytes) bmp = frdp_dib_to_bmp_file (dib);
  const guint8 *p = g_bytes_get_data (bmp, NULL);

  g_assert_cmpuint (g_bytes_get_size (bmp), ==, 14 + g_bytes_get_size (dib));
  g_assert_cmpint (p[0], ==, 'B');
  g_assert_cmpint (p[1], ==, 'M');
  g_assert_cmpuint (p[10], ==, 54);   /* pixel data offset = 14 + 40 */
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/dib/round-trip", test_round_trip);
  g_test_add_func ("/dib/24bpp-top-down", test_24bpp_top_down);
  g_test_add_func ("/dib/v5-bitfields", test_v5_bitfields);
  g_test_add_func ("/dib/rejects-bad-input", test_rejects_bad_input);
  g_test_add_func ("/dib/bmp-file", test_bmp_file);
  return g_test_run ();
}
