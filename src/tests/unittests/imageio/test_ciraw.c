/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

// the Apple Core Image RAW v9 loader: field contract of a v9 decode,
// flags after the decode, determinism across two decodes, and the
// best-available-version fallback for a camera outside the v9 list.
// Only built with USE_CIRAW.
//
// needs darktable started: dt_imageio_open_ciraw reads EXIF (which locks
// darktable.exiv2_threadsafe) and renders into a mipmap buffer, so the
// suite init is dt_init on an in-memory library like darktable-cli's.

#include "common/cache.h"
#include "common/darktable.h"
#include "common/image.h"
#include "common/mipmap_cache.h"
#include "imageio/imageio_ciraw.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#ifndef CIRAW_TEST_SAMPLE
#define CIRAW_TEST_SAMPLE ""
#endif
#ifndef CIRAW_TEST_NONV9_SAMPLE
#define CIRAW_TEST_NONV9_SAMPLE ""
#endif

// the exit status ctest is told means "skipped" (SKIP_RETURN_CODE)
#define CIRAW_SKIP_RC 77

static gchar *_tmpdir = NULL;

static int _group_setup(void **state)
{
  _tmpdir = g_dir_make_tmp("dt-ciraw-test-XXXXXX", NULL);
  if(!_tmpdir) return -1;

  char *argv[] = { "darktable-test",
                   "--library", ":memory:",
                   "--configdir", _tmpdir,
                   "--cachedir", _tmpdir,
                   "--datadir", DT_TEST_DATADIR,
                   "--moduledir", DT_TEST_MODULEDIR,
                   "--disable-opencl",
                   "--conf", "write_sidecar_files=never",
                   NULL };
  const int argc = sizeof(argv) / sizeof(argv[0]) - 1;
  return dt_init(argc, argv, FALSE, FALSE, NULL);
}

static void _rm_rf(const char *path)
{
  GDir *d = g_dir_open(path, 0, NULL);
  if(d)
  {
    const gchar *name;
    while((name = g_dir_read_name(d)))
    {
      gchar *full = g_build_filename(path, name, NULL);
      if(g_file_test(full, G_FILE_TEST_IS_DIR)
         && !g_file_test(full, G_FILE_TEST_IS_SYMLINK))
        _rm_rf(full);
      else
        g_remove(full);
      g_free(full);
    }
    g_dir_close(d);
  }
  g_rmdir(path);
}

static int _group_teardown(void **state)
{
  dt_cleanup();
  if(_tmpdir)
  {
    // configdir and cachedir hold darktablerc, data.db and mipmap folders
    _rm_rf(_tmpdir);
    const int rc = g_file_test(_tmpdir, G_FILE_TEST_EXISTS) ? -1 : 0;
    g_free(_tmpdir);
    _tmpdir = NULL;
    return rc;
  }
  return 0;
}

static void _free_entry(dt_cache_entry_t *entry)
{
  if(entry->data) dt_free_align(entry->data);
  g_free(entry);
}

// a dt_mipmap_buffer_t shaped like dt_mipmap_cache_get() leaves one, minus
// the cache: dt_mipmap_cache_alloc only touches buf->size, buf->cache_entry
// and buf->buf, and frees a previous entry->data with dt_free_align
static void _buf_init(dt_mipmap_buffer_t *mbuf, dt_cache_entry_t **entry)
{
  *entry = g_new0(dt_cache_entry_t, 1);
  mbuf->size = DT_MIPMAP_FULL;
  mbuf->imgid = 0;
  mbuf->buf = NULL;
  mbuf->cache_entry = *entry;
}

static void test_probe_fills_the_field_contract(void **state)
{
  // probe call (mbuf == NULL): metadata only, valid extent, RGB float
  // descriptor, Apple RAW flags, and the CIRAW loader tag
  dt_image_t img;
  dt_image_init(&img);
  g_strlcpy(img.filename, CIRAW_TEST_SAMPLE, sizeof(img.filename));

  assert_int_equal(dt_imageio_open_ciraw(&img, CIRAW_TEST_SAMPLE, NULL),
                   DT_IMAGEIO_OK);
  assert_int_equal(img.loader, LOADER_CIRAW);
  assert_int_equal(img.buf_dsc.channels, 4);
  assert_int_equal(img.buf_dsc.datatype, TYPE_FLOAT);
  assert_int_equal(img.buf_dsc.cst, IOP_CS_RGB);
  assert_true(img.width > 0);
  assert_true(img.height > 0);
  assert_true(dt_image_is_ciraw(&img));
  assert_false(img.flags & DT_IMAGE_RAW);
  assert_true(img.flags & DT_IMAGE_HDR);
  assert_int_equal(img.raw_white_point, 1);
}

static gboolean _decode(dt_image_t *img,
                        dt_mipmap_buffer_t *mbuf,
                        float **pixels)
{
  dt_image_init(img);
  g_strlcpy(img->filename, CIRAW_TEST_SAMPLE, sizeof(img->filename));
  if(dt_imageio_open_ciraw(img, CIRAW_TEST_SAMPLE, mbuf) != DT_IMAGEIO_OK)
    return FALSE;
  *pixels = (float *)mbuf->buf;
  return *pixels != NULL;
}

static void test_full_decode_renders_float_rgb(void **state)
{
  dt_image_t img;
  dt_cache_entry_t *entry;
  dt_mipmap_buffer_t mbuf;
  _buf_init(&mbuf, &entry);

  float *pixels = NULL;
  assert_true(_decode(&img, &mbuf, &pixels));
  assert_int_equal(img.loader, LOADER_CIRAW);
  // alpha is pinned to 0 like the sraw path, so every fourth float is exact
  const size_t n = (size_t)img.width * img.height;
  assert_true(n > 0);
  for(size_t k = 0; k < n; k += n / 257 + 1)
    assert_true(pixels[k * 4 + 3] == 0.0f);
  _free_entry(entry);
}

static void test_two_decodes_are_bit_identical(void **state)
{
  dt_image_t img1, img2;
  dt_cache_entry_t *entry1, *entry2;
  dt_mipmap_buffer_t mbuf1, mbuf2;
  _buf_init(&mbuf1, &entry1);
  _buf_init(&mbuf2, &entry2);

  float *p1 = NULL, *p2 = NULL;
  assert_true(_decode(&img1, &mbuf1, &p1));
  assert_true(_decode(&img2, &mbuf2, &p2));
  assert_int_equal(img1.width, img2.width);
  assert_int_equal(img1.height, img2.height);
  assert_int_equal(
      memcmp(p1, p2, (size_t)img1.width * img1.height * 4 * sizeof(float)), 0);
  _free_entry(entry1);
  _free_entry(entry2);
}

static void test_non_v9_camera_decodes_best_available(void **state)
{
  if(!CIRAW_TEST_NONV9_SAMPLE[0]
     || !g_file_test(CIRAW_TEST_NONV9_SAMPLE, G_FILE_TEST_IS_REGULAR))
    skip();

  dt_image_t img;
  dt_image_init(&img);
  g_strlcpy(img.filename, CIRAW_TEST_NONV9_SAMPLE, sizeof(img.filename));

  // outside the v9 list is a downgrade to the newest legacy version,
  // not an error: the probe must succeed and mark the image CIRAW
  assert_int_equal(
      dt_imageio_open_ciraw(&img, CIRAW_TEST_NONV9_SAMPLE, NULL),
      DT_IMAGEIO_OK);
  assert_true(img.flags & DT_IMAGE_CIRAW);
  assert_int_equal(img.loader, LOADER_CIRAW);
  assert_true(img.width > 0);
  assert_true(img.height > 0);
}

int main(int argc,
         char *argv[])
{
  if(!CIRAW_TEST_SAMPLE[0]
     || !g_file_test(CIRAW_TEST_SAMPLE, G_FILE_TEST_IS_REGULAR))
  {
    printf("no CIRAW v9 sample configured (CIRAW_TEST_SAMPLE) -- skipping\n");
    return CIRAW_SKIP_RC;
  }

  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_probe_fills_the_field_contract),
    cmocka_unit_test(test_full_decode_renders_float_rgb),
    cmocka_unit_test(test_two_decodes_are_bit_identical),
    cmocka_unit_test(test_non_v9_camera_decodes_best_available),
  };
  return cmocka_run_group_tests(tests, _group_setup, _group_teardown);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
