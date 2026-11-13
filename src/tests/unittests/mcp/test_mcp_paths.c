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

/* white-box tests for the MCP bridge's path and name handling.
 *
 * these helpers decide which file on disk a request refers to and which file an
 * export writes. every serious defect found in the MCP server so far has been
 * here: a path spelling that slipped past the "is this already in the catalog?"
 * probe imported a duplicate row and, earlier, deleted the user's image when the
 * scratch row was cleaned up again. the cases below are the spellings that
 * actually broke it, so a future change that reopens one of them fails here
 * rather than in someone's catalog.
 *
 * following test_filmicrgb.c, the implementation is #included directly for
 * access to its static functions; lib_darktable supplies what it links against.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "mcp/dt_bridge.c"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

// a scratch directory per test run, removed in teardown
static gchar *_tmpdir = NULL;

static int _setup(void **state)
{
  (void)state;
  GError *e = NULL;
  _tmpdir = g_dir_make_tmp("dt-mcp-paths-XXXXXX", &e);
  if(!_tmpdir)
  {
    if(e) g_error_free(e);
    return -1;
  }
#ifdef _WIN32
  // _canonical_path() expands 8.3 short names through GetLongPathNameW(), so
  // the expected paths must be built on the long form: %TMP% holds a short
  // one whenever a profile name exceeds eight characters
  gchar *lng = dt_util_normalize_path(_tmpdir);
  if(lng)
  {
    g_free(_tmpdir);
    _tmpdir = lng;
  }
#else
  // _canonical_path() resolves symlinks, so the expected paths must be built on
  // the resolved directory: on macOS the temp dir lives under /var, which is a
  // symlink to /private/var
  char *real = realpath(_tmpdir, NULL);
  if(real)
  {
    g_free(_tmpdir);
    _tmpdir = g_strdup(real);
    free(real);
  }
#endif
  return 0;
}

static void _rm_rf(const char *path)
{
  GDir *d = g_dir_open(path, 0, NULL);
  if(d)
  {
    const char *name;
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

static int _teardown(void **state)
{
  (void)state;
  if(_tmpdir)
  {
    _rm_rf(_tmpdir);
    g_free(_tmpdir);
    _tmpdir = NULL;
  }
  return 0;
}

static void _touch(const char *path)
{
  gchar *dir = g_path_get_dirname(path);
  g_mkdir_with_parents(dir, 0755);
  g_free(dir);
  g_file_set_contents(path, "x", 1, NULL);
}

// ---------------------------------------------------------------------------
// _canonical_path
// ---------------------------------------------------------------------------

// the spelling that imported a duplicate row and a film roll displayed as "."
static void test_canonical_dot_segment(void **state)
{
  (void)state;
  gchar *in = g_build_filename(_tmpdir, ".", "a.raw", NULL);
  gchar *want = g_build_filename(_tmpdir, "a.raw", NULL);
  _touch(want);

  gchar *got = _canonical_path(in);
  assert_non_null(got);
  assert_string_equal(got, want);

  g_free(got); g_free(want); g_free(in);
}

static void test_canonical_parent_segment(void **state)
{
  (void)state;
  gchar *sub = g_build_filename(_tmpdir, "sub", NULL);
  g_mkdir_with_parents(sub, 0755);
  gchar *file = g_build_filename(_tmpdir, "b.raw", NULL);
  _touch(file);

  gchar *in = g_build_filename(sub, "..", "b.raw", NULL);
  gchar *got = _canonical_path(in);
  assert_non_null(got);
  assert_string_equal(got, file);

  g_free(got); g_free(in); g_free(file); g_free(sub);
}

static void test_canonical_double_separator(void **state)
{
  (void)state;
  gchar *file = g_build_filename(_tmpdir, "c.raw", NULL);
  _touch(file);
  gchar *in = g_strdup_printf("%s//c.raw", _tmpdir);

  gchar *got = _canonical_path(in);
  assert_non_null(got);
  assert_string_equal(got, file);

  g_free(got); g_free(in); g_free(file);
}

// dt_util_normalize_path() handles the URI form; canonicalization must survive it
static void test_canonical_file_uri(void **state)
{
  (void)state;
  gchar *file = g_build_filename(_tmpdir, "d.raw", NULL);
  _touch(file);
  gchar *uri = g_strdup_printf("file://%s/./d.raw", _tmpdir);

  gchar *got = _canonical_path(uri);
  assert_non_null(got);
  assert_string_equal(got, file);

  g_free(got); g_free(uri); g_free(file);
}

// a symlinked parent is a different string for the same file, and the probe
// matches film_rolls.folder exactly
static void test_canonical_symlinked_parent(void **state)
{
  (void)state;
#ifdef _WIN32
  // mingw-w64 provides no symlink(), so there is nothing to build a link with
  skip();
#else
  gchar *real = g_build_filename(_tmpdir, "realdir", NULL);
  g_mkdir_with_parents(real, 0755);
  gchar *file = g_build_filename(real, "e.raw", NULL);
  _touch(file);

  gchar *link = g_build_filename(_tmpdir, "linkdir", NULL);
  if(symlink(real, link) != 0)   // no symlink support: nothing to assert
  {
    g_free(link); g_free(file); g_free(real);
    skip();
  }

  gchar *in = g_build_filename(link, "e.raw", NULL);
  gchar *got = _canonical_path(in);
  assert_non_null(got);
  assert_string_equal(got, file);

  g_free(got); g_free(in); g_free(link); g_free(file); g_free(real);
#endif
}

// ".." must not walk above the root
static void test_canonical_cannot_escape_root(void **state)
{
  (void)state;
#ifdef _WIN32
  // dt_util_normalize_path() resolves through GetLongPathNameW(), which needs
  // the path to exist; "/../../.." never does
  skip();
#else
  gchar *got = _canonical_path("/../../..");
  assert_non_null(got);
  assert_string_equal(got, G_DIR_SEPARATOR_S);
  g_free(got);
#endif
}

// an already-canonical path must come back untouched, or every probe shifts
static void test_canonical_is_idempotent(void **state)
{
  (void)state;
  gchar *file = g_build_filename(_tmpdir, "f.raw", NULL);
  _touch(file);

  gchar *once = _canonical_path(file);
  assert_non_null(once);
  gchar *twice = _canonical_path(once);
  assert_non_null(twice);
  assert_string_equal(once, file);
  assert_string_equal(twice, once);

  g_free(twice); g_free(once); g_free(file);
}

// a path naming no existing file still canonicalizes: the probe runs before the
// file-exists check, so realpath() failing must not lose the lexical result
static void test_canonical_missing_file(void **state)
{
  (void)state;
#ifdef _WIN32
  // GetLongPathNameW() fails on a path that does not exist, so a missing file
  // cannot be canonicalized here
  skip();
#else
  gchar *in = g_build_filename(_tmpdir, ".", "nope.raw", NULL);
  gchar *want = g_build_filename(_tmpdir, "nope.raw", NULL);

  gchar *got = _canonical_path(in);
  assert_non_null(got);
  assert_string_equal(got, want);

  g_free(got); g_free(want); g_free(in);
#endif
}

// ---------------------------------------------------------------------------
// _sql_like_escape
// ---------------------------------------------------------------------------

// '_' matched any character, so a folder filter of "Pictures_MCP" also matched
// "Pictures/MCP"
static void test_like_escape(void **state)
{
  (void)state;
  struct { const char *in, *want; } cases[] = {
    { "plain",      "plain"       },
    { "a_b",        "a\\_b"       },
    { "100%",       "100\\%"      },
    { "a\\b",       "a\\\\b"      },
    { "_%\\",       "\\_\\%\\\\"  },
    { "",           ""            },
  };
  for(size_t i = 0; i < G_N_ELEMENTS(cases); i++)
  {
    gchar *got = _sql_like_escape(cases[i].in);
    assert_non_null(got);
    assert_string_equal(got, cases[i].want);
    g_free(got);
  }
}

// ---------------------------------------------------------------------------
// _film_images_since
// ---------------------------------------------------------------------------

// dt_image_import() files a row per "<name>_NN.<ext>.xmp" sidecar besides the
// image (image.c:2068); cleanup used to remove only the id it handed back, so
// those rows and their film roll survived every render of a loose file.
// covers the query alone: the _import_file() -> _drop_scratch() wiring needs a
// booted darktable with an open library, which this binary has not
static void test_film_images_since_lists_duplicates(void **state)
{
  (void)state;
  sqlite3 *db = NULL;
  assert_int_equal(sqlite3_open(":memory:", &db), SQLITE_OK);
  assert_int_equal(sqlite3_exec(db,
      "CREATE TABLE main.images (id INTEGER PRIMARY KEY, film_id INTEGER,"
      "                          filename TEXT, version INTEGER)",
      NULL, NULL, NULL), SQLITE_OK);

  // the catalog as it stood: another roll, and one image already in ours
  assert_int_equal(sqlite3_exec(db,
      "INSERT INTO main.images VALUES (10, 1, 'other.raw', 0),"
      "                               (11, 2, 'pre.raw', 0)",
      NULL, NULL, NULL), SQLITE_OK);
  const dt_imgid_t high = 11;   // what _max_image_id() reads before the import

  // what one import adds: the image, then a row per sidecar duplicate
  assert_int_equal(sqlite3_exec(db,
      "INSERT INTO main.images VALUES (12, 2, 'a.raw', 0),"
      "                               (13, 2, 'a.raw', 1),"
      "                               (14, 2, 'a.raw', 2)",
      NULL, NULL, NULL), SQLITE_OK);

  GList *got = _film_images_since(db, 2, high);
  assert_int_equal(g_list_length(got), 3);
  const int want[] = { 12, 13, 14 };
  int i = 0;
  for(GList *l = got; l; l = g_list_next(l), i++)
    assert_int_equal(GPOINTER_TO_INT(l->data), want[i]);
  g_list_free(got);

  // an import that added nothing owns nothing
  assert_null(_film_images_since(db, 2, 14));
  // and a roll this request never touched is never swept
  assert_null(_film_images_since(db, 1, high));

  // 0 is a real baseline: an empty library, so every row in the roll is ours
  GList *all = _film_images_since(db, 2, 0);
  assert_int_equal(g_list_length(all), 4);   // 11 was already there
  g_list_free(all);

  // -1 is _max_image_id() saying it could not answer. it must not read as 0:
  // _drop_scratch() would delete the rows the user had before the import
  assert_null(_film_images_since(db, 2, -1));

  sqlite3_close(db);
}

// the sentinel above only helps if the failure path actually produces it. with
// no library open the query cannot run, and returning 0 would claim the
// catalog was empty
static void test_max_image_id_without_database(void **state)
{
  (void)state;
  assert_null(darktable.db);   // the precondition this test is about
  assert_int_equal(_max_image_id(), -1);
}

// ---------------------------------------------------------------------------
// _image_is_undeveloped
// ---------------------------------------------------------------------------

// this decides which images --read-only may roll back, and has to be exact both
// ways: a false yes deletes real edits, a false no leaves read-only having
// developed the image. the end-to-end path needs a booted darktable
static void test_undeveloped_only_when_nothing_is_filed(void **state)
{
  (void)state;
  sqlite3 *db = NULL;
  assert_int_equal(sqlite3_open(":memory:", &db), SQLITE_OK);
  assert_int_equal(sqlite3_exec(db,
      "CREATE TABLE main.images (id INTEGER PRIMARY KEY, history_end INTEGER,"
      "                          flags INTEGER);"
      "CREATE TABLE main.history (imgid INTEGER, num INTEGER);"
      "CREATE TABLE main.masks_history (imgid INTEGER, num INTEGER);"
      "CREATE TABLE main.module_order (imgid INTEGER, version INTEGER);"
      "CREATE TABLE main.history_hash (imgid INTEGER, current_hash BLOB)",
      NULL, NULL, NULL), SQLITE_OK);

  // 1: never developed. 2: what a first render leaves. 3-6: one row in each
  // table the rollback would empty. 7: NULL history_end, which the catalog
  // treats as 0. 9: presets applied but nothing added, what workflow "none"
  // leaves (develop.c:2246 sets the flag even with no module inserted)
  assert_int_equal(sqlite3_exec(db,
      "INSERT INTO main.images VALUES (1, 0, 1089), (2, 11, 1601), (3, 0, 1089),"
      "                               (4, 0, 1089), (5, 0, 1089), (6, 0, 1089),"
      "                               (7, NULL, 1089), (8, 11, 1089), (9, 0, 1601);"
      "INSERT INTO main.history VALUES (3, 0);"
      "INSERT INTO main.masks_history VALUES (4, 0);"
      "INSERT INTO main.module_order VALUES (5, 4);"
      "INSERT INTO main.history_hash VALUES (6, NULL)",
      NULL, NULL, NULL), SQLITE_OK);

  assert_true(_image_is_undeveloped(db, 1));
  assert_true(_image_is_undeveloped(db, 7));

  assert_false(_image_is_undeveloped(db, 2));   // presets already applied
  assert_false(_image_is_undeveloped(db, 3));   // has history
  assert_false(_image_is_undeveloped(db, 4));   // has a mask
  assert_false(_image_is_undeveloped(db, 5));   // has a module order
  assert_false(_image_is_undeveloped(db, 6));   // has a history hash
  assert_false(_image_is_undeveloped(db, 8));   // history_end past the rows
  // clearing this flag would have darktable auto-apply all over again
  assert_false(_image_is_undeveloped(db, 9));

  // an id the catalog does not know is not an image to roll back
  assert_false(_image_is_undeveloped(db, 99));

  sqlite3_close(db);
}

// a query that cannot run must read as "developed": the rollback deletes rows,
// so it may only ever act on a state it has actually read back
static void test_undeveloped_without_database(void **state)
{
  (void)state;
  assert_false(_image_is_undeveloped(NULL, 1));

  sqlite3 *db = NULL;
  assert_int_equal(sqlite3_open(":memory:", &db), SQLITE_OK);
  assert_false(_image_is_undeveloped(db, 1));   // no tables to prepare against
  sqlite3_close(db);
}

// ---------------------------------------------------------------------------
// _resolve_conflict
// ---------------------------------------------------------------------------

static void test_conflict_free_name_passthrough(void **state)
{
  (void)state;
  // no file there: returned as given, and this path never consults the config
  gchar *want = g_build_filename(_tmpdir, "free.jpg", NULL);
  gboolean skipped = TRUE;
  gchar *got = _resolve_conflict(want, 0, NULL, &skipped);
  assert_non_null(got);
  assert_false(skipped);
  assert_string_equal(got, want);
  g_free(got); g_free(want);
}

static void test_conflict_unique_name(void **state)
{
  (void)state;
  gchar *base = g_build_filename(_tmpdir, "clash.jpg", NULL);
  _touch(base);

  gboolean skipped = TRUE;
  gchar *first = _resolve_conflict(base, 0, NULL, &skipped);
  assert_non_null(first);
  assert_false(skipped);
  gchar *want1 = g_build_filename(_tmpdir, "clash_01.jpg", NULL);
  assert_string_equal(first, want1);

  // and it keeps counting rather than reusing _01
  _touch(first);
  gchar *second = _resolve_conflict(base, 0, NULL, &skipped);
  assert_non_null(second);
  gchar *want2 = g_build_filename(_tmpdir, "clash_02.jpg", NULL);
  assert_string_equal(second, want2);

  g_free(second); g_free(want2); g_free(first); g_free(want1); g_free(base);
}

// the extension was split over the whole path, so a directory containing a dot
// sent the retry into a different, newly created folder
static void test_conflict_dotted_directory(void **state)
{
  (void)state;
  gchar *dir = g_build_filename(_tmpdir, "has.dot", NULL);
  g_mkdir_with_parents(dir, 0755);
  gboolean skipped = TRUE;

  // with an extension the last dot of the whole path is the right one anyway,
  // so this alone would not notice the split being done over the full string
  gchar *base = g_build_filename(dir, "g.jpg", NULL);
  _touch(base);
  gchar *got = _resolve_conflict(base, 0, NULL, &skipped);
  assert_non_null(got);
  gchar *want = g_build_filename(dir, "g_01.jpg", NULL);
  assert_string_equal(got, want);
  g_free(want); g_free(got); g_free(base);

  // the case that actually breaks: no dot in the basename, so a split over the
  // whole path lands on the directory's dot and the retry moves to a sibling
  // directory that does not exist
  gchar *noext = g_build_filename(dir, "plain", NULL);
  _touch(noext);
  got = _resolve_conflict(noext, 0, NULL, &skipped);
  assert_non_null(got);
  want = g_build_filename(dir, "plain_01", NULL);
  assert_string_equal(got, want);

  // whatever it picked must stay inside the directory it started in
  gchar *got_dir = g_path_get_dirname(got);
  assert_string_equal(got_dir, dir);

  g_free(got_dir); g_free(want); g_free(got); g_free(noext); g_free(dir);
}

static void test_conflict_overwrite_and_skip(void **state)
{
  (void)state;
  gchar *base = g_build_filename(_tmpdir, "policy.jpg", NULL);
  _touch(base);
  gboolean skipped = TRUE;

  gchar *over = _resolve_conflict(base, 1, NULL, &skipped);   // overwrite
  assert_non_null(over);
  assert_false(skipped);
  assert_string_equal(over, base);
  g_free(over);

  gchar *skip_out = _resolve_conflict(base, 3, NULL, &skipped);   // skip
  assert_null(skip_out);
  assert_true(skipped);

  g_free(base);
}

// colliding basenames resolve their targets before either file is written, so
// the filesystem cannot warn the second. losing an image is never what a policy
// asked for, so an in-batch collision takes a free name under every policy
static void _check_batch_collision(const int action)
{
  gchar *dir = g_strdup_printf("%s/batch%d", _tmpdir, action);
  g_mkdir_with_parents(dir, 0755);
  GHashTable *claimed =
    g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  gchar *base = g_build_filename(dir, "dup.jpg", NULL);
  gboolean skipped = TRUE;

  gchar *first = _resolve_conflict(base, action, claimed, &skipped);
  assert_non_null(first);
  assert_false(skipped);
  assert_string_equal(first, base);
  g_hash_table_add(claimed, g_strdup(first));

  // nothing has been written yet: only `claimed` knows this name is taken
  assert_false(g_file_test(base, G_FILE_TEST_EXISTS));
  gchar *second = _resolve_conflict(base, action, claimed, &skipped);
  assert_non_null(second);
  assert_false(skipped);
  gchar *want = g_build_filename(dir, "dup_01.jpg", NULL);
  assert_string_equal(second, want);
  g_hash_table_add(claimed, g_strdup(second));

  // and it keeps counting rather than handing _01 out twice
  gchar *third = _resolve_conflict(base, action, claimed, &skipped);
  assert_non_null(third);
  gchar *want2 = g_build_filename(dir, "dup_02.jpg", NULL);
  assert_string_equal(third, want2);

  g_free(want2); g_free(third); g_free(want); g_free(second);
  g_free(first); g_free(base);
  g_hash_table_destroy(claimed);
  g_free(dir);
}

static void test_conflict_in_batch_collision(void **state)
{
  (void)state;
  for(int action = 0; action <= 3; action++)
    _check_batch_collision(action);
}

// a file that was already on disk and a name the batch has claimed are both
// taken: the retry loop has to step over either
static void test_conflict_in_batch_skips_existing(void **state)
{
  (void)state;
  GHashTable *claimed =
    g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  gchar *base = g_build_filename(_tmpdir, "both.jpg", NULL);
  _touch(base);
  gboolean skipped = TRUE;

  gchar *first = _resolve_conflict(base, 0, claimed, &skipped);
  gchar *want1 = g_build_filename(_tmpdir, "both_01.jpg", NULL);
  assert_non_null(first);
  assert_string_equal(first, want1);
  g_hash_table_add(claimed, g_strdup(first));

  gchar *second = _resolve_conflict(base, 0, claimed, &skipped);
  gchar *want2 = g_build_filename(_tmpdir, "both_02.jpg", NULL);
  assert_non_null(second);
  assert_string_equal(second, want2);

  g_free(want2); g_free(second); g_free(want1); g_free(first); g_free(base);
  g_hash_table_destroy(claimed);
}

// ---------------------------------------------------------------------------
// char-array (string) params
// ---------------------------------------------------------------------------

// the introspection-driven param tools need a module to work on, and booting
// darktable for one is out of scope here. this fake mirrors what the perl
// parser emits for `float x; char label[8];`: a linear FLOAT entry, a linear
// CHAR entry named "label[0]", and an ARRAY entry whose count is the capacity
// and whose field points back at the CHAR entry. its get_f() resolves only the
// exact emitted names, so _resolve_field()'s bare-array-name fallback is what
// is actually under test
typedef struct { float amount; char label[8]; } _fake_params_t;

static dt_introspection_field_t _fake_linear[4];

static dt_introspection_t _fake_intro = {
  .api_version = DT_INTROSPECTION_VERSION,
  .params_version = 1,
  .size = sizeof(_fake_params_t),
};

static void _fake_build(void)
{
  static const char *const label_name = "label";
  static const char *const label_elem = "label[0]";
  static const char *const amount_name = "amount";

  memset(_fake_linear, 0, sizeof(_fake_linear));
  _fake_linear[0].Float.header.type = DT_INTROSPECTION_TYPE_FLOAT;
  _fake_linear[0].Float.header.name = amount_name;
  _fake_linear[0].Float.header.field_name = amount_name;
  _fake_linear[0].Float.header.size = sizeof(float);
  _fake_linear[0].Float.header.offset = offsetof(_fake_params_t, amount);
  _fake_linear[0].Float.Min = -10.0f;
  _fake_linear[0].Float.Max = 10.0f;
  _fake_linear[1].Char.header.type = DT_INTROSPECTION_TYPE_CHAR;
  _fake_linear[1].Char.header.name = label_elem;
  _fake_linear[1].Char.header.field_name = label_elem;
  _fake_linear[1].Char.header.size = 1;
  _fake_linear[1].Char.header.offset = offsetof(_fake_params_t, label);
  _fake_linear[2].Array.header.type = DT_INTROSPECTION_TYPE_ARRAY;
  _fake_linear[2].Array.header.name = label_name;
  _fake_linear[2].Array.header.field_name = label_name;
  _fake_linear[2].Array.header.size = sizeof(((_fake_params_t *)0)->label);
  _fake_linear[2].Array.header.offset = offsetof(_fake_params_t, label);
  _fake_linear[2].Array.count = G_N_ELEMENTS(((_fake_params_t *)0)->label);
  _fake_linear[2].Array.type = DT_INTROSPECTION_TYPE_CHAR;
  _fake_linear[2].Array.field = &_fake_linear[1];
  _fake_linear[3].header.type = DT_INTROSPECTION_TYPE_NONE;
  _fake_intro.field = NULL;
}

// resolves only the emitted names: "amount" and "label[0]", not bare "label"
static dt_introspection_field_t *_fake_get_f(const char *name)
{
  for(int i = 0; i < 3; i++)
    if(!strcmp(_fake_linear[i].header.name, name)) return &_fake_linear[i];
  return NULL;
}

// mirrors the generated getter: the bare array name and its [0] element both
// reach the same bytes
static void *_fake_get_p(const void *param, const char *name)
{
  _fake_params_t *p = (_fake_params_t *)param;
  if(!strcmp(name, "amount")) return &p->amount;
  if(!strcmp(name, "label") || !strcmp(name, "label[0]")) return p->label;
  return NULL;
}

static dt_introspection_t *_fake_get_intro(void) { return &_fake_intro; }
static dt_introspection_field_t *_fake_get_linear(void) { return _fake_linear; }

static dt_iop_module_so_t *_fake_so(void)
{
  static dt_iop_module_so_t so;
  memset(&so, 0, sizeof(so));
  g_strlcpy(so.op, "fake", sizeof(so.op));
  so.have_introspection = TRUE;
  so.get_introspection = _fake_get_intro;
  so.get_introspection_linear = _fake_get_linear;
  so.get_f = _fake_get_f;
  so.get_p = _fake_get_p;
  return &so;
}

// build a one-field JsonObject parsed from JSON, values as strings or numbers
static JsonObject *_fields_from(const char *json_text)
{
  JsonParser *p = json_parser_new();
  json_parser_load_from_data(p, json_text, -1, NULL);
  JsonNode *root = json_parser_get_root(p);
  JsonObject *o = json_node_get_object(root);
  json_object_ref(o);          // survive the parser being freed
  g_object_unref(p);
  return o;
}

// pull one string field back out of a _write_fields_object JSON document
static char *_decoded_field(const char *json, const char *field)
{
  JsonParser *p = json_parser_new();
  json_parser_load_from_data(p, json, -1, NULL);
  JsonNode *root = json_parser_get_root(p);
  JsonObject *o = json_node_get_object(root);
  char *out = NULL;
  if(o && json_object_has_member(o, field))
  {
    JsonNode *v = json_object_get_member(o, field);
    if(JSON_NODE_HOLDS_VALUE(v) && json_node_get_value_type(v) == G_TYPE_STRING)
      out = g_strdup(json_node_get_string(v));
  }
  g_object_unref(p);
  return out;
}

static void test_char_capacity_counts_the_array(void **state)
{
  (void)state;
  _fake_build();
  dt_iop_module_so_t *so = _fake_so();

  assert_int_equal(_char_capacity(so, &_fake_linear[0]), 0);   // float
  assert_int_equal(_char_capacity(so, &_fake_linear[1]), 8);   // label[0]
}

// the bare array name resolves to the CHAR element the parser emitted
static void test_resolve_field_accepts_bare_array_name(void **state)
{
  (void)state;
  _fake_build();
  dt_iop_module_so_t *so = _fake_so();

  assert_ptr_equal(_resolve_field(so, "amount"), &_fake_linear[0]);
  assert_ptr_equal(_resolve_field(so, "label[0]"), &_fake_linear[1]);
  assert_ptr_equal(_resolve_field(so, "label"), &_fake_linear[1]);
  assert_null(_resolve_field(so, "nope"));
}

// set by name, read back by name: the round trip the framing module needs for
// text_model / text_font / film_caption
static void test_char_field_round_trip(void **state)
{
  (void)state;
  _fake_build();
  dt_iop_module_so_t *so = _fake_so();

  JsonObject *fields = _fields_from("{\"label\": \"KODAK\"}");
  size_t size = 0;
  char *err = NULL;
  uint8_t *blob = _seed_and_apply(so, NULL, fields, &size, &err);
  assert_non_null(blob);
  assert_null(err);
  assert_int_equal(size, sizeof(_fake_params_t));

  _fake_params_t *p = (_fake_params_t *)blob;
  assert_string_equal(p->label, "KODAK");
  // every byte behind the NUL stays zero, so the blob hashes deterministically
  for(int i = 6; i < 8; i++) assert_int_equal(p->label[i], 0);

  // and the decoder reads the string back, byte-exact
  JsonBuilder *b = json_builder_new();
  _write_fields_object(so, blob, b);
  char *json = _builder_to_string(b);
  g_object_unref(b);
  char *got = _decoded_field(json, "label[0]");
  assert_string_equal(got, "KODAK");

  g_free(got); g_free(json); g_free(blob);
  json_object_unref(fields);
}

// an over-long string is cut at whole utf-8 characters with the NUL kept, not
// silently clamped mid-sequence
static void test_char_field_truncates_on_utf8_boundary(void **state)
{
  (void)state;
  _fake_build();
  dt_iop_module_so_t *so = _fake_so();

  // four "ü" (2 bytes each) = 8 bytes; capacity 8 fits 7 bytes, so 3 chars
  JsonObject *fields = _fields_from("{\"label\": \"üüüü\"}");
  size_t size = 0;
  char *err = NULL;
  uint8_t *blob = _seed_and_apply(so, NULL, fields, &size, &err);
  assert_non_null(blob);
  assert_null(err);

  _fake_params_t *p = (_fake_params_t *)blob;
  assert_string_equal(p->label, "üüü");
  assert_int_equal(p->label[6], '\0');
  assert_int_equal(p->label[7], 0);

  g_free(blob);
  json_object_unref(fields);
}

// a shorter string must clear the bytes a longer one had left behind
static void test_char_field_shorter_string_zeroes_tail(void **state)
{
  (void)state;
  _fake_build();
  dt_iop_module_so_t *so = _fake_so();

  JsonObject *long_fields = _fields_from("{\"label\": \"KODAK\"}");
  size_t size = 0;
  uint8_t *blob = _seed_and_apply(so, NULL, long_fields, &size, NULL);
  assert_non_null(blob);
  json_object_unref(long_fields);

  // reuse the blob as the defaults source, as _apply_entry() does with a
  // module's live params: the seeded bytes carry the old string
  JsonObject *short_fields = _fields_from("{\"label\": \"ab\"}");
  uint8_t *rebuilt = _seed_and_apply(so, blob, short_fields, &size, NULL);
  assert_non_null(rebuilt);

  _fake_params_t *p = (_fake_params_t *)rebuilt;
  assert_string_equal(p->label, "ab");
  for(int i = 3; i < 8; i++) assert_int_equal(p->label[i], 0);

  g_free(rebuilt); g_free(blob);
  json_object_unref(short_fields);
}

// a non-string value for a char field is refused, not coerced
static void test_char_field_refuses_numbers(void **state)
{
  (void)state;
  _fake_build();
  dt_iop_module_so_t *so = _fake_so();

  JsonObject *fields = _fields_from("{\"label\": 5}");
  size_t size = 0;
  char *err = NULL;
  uint8_t *blob = _seed_and_apply(so, NULL, fields, &size, &err);
  assert_null(blob);
  assert_non_null(err);
  assert_true(strstr(err, "expects a string") != NULL);

  g_free(err);
  json_object_unref(fields);
}

// an unknown field still fails the same way it always did
static void test_unknown_field_still_refused(void **state)
{
  (void)state;
  _fake_build();
  dt_iop_module_so_t *so = _fake_so();

  JsonObject *fields = _fields_from("{\"nope\": \"x\"}");
  size_t size = 0;
  char *err = NULL;
  uint8_t *blob = _seed_and_apply(so, NULL, fields, &size, &err);
  assert_null(blob);
  assert_non_null(err);
  assert_true(strstr(err, "unknown or non-scalar field") != NULL);

  g_free(err);
  json_object_unref(fields);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_canonical_dot_segment),
    cmocka_unit_test(test_canonical_parent_segment),
    cmocka_unit_test(test_canonical_double_separator),
    cmocka_unit_test(test_canonical_file_uri),
    cmocka_unit_test(test_canonical_symlinked_parent),
    cmocka_unit_test(test_canonical_cannot_escape_root),
    cmocka_unit_test(test_canonical_is_idempotent),
    cmocka_unit_test(test_canonical_missing_file),
    cmocka_unit_test(test_like_escape),
    cmocka_unit_test(test_film_images_since_lists_duplicates),
    cmocka_unit_test(test_max_image_id_without_database),
    cmocka_unit_test(test_undeveloped_only_when_nothing_is_filed),
    cmocka_unit_test(test_undeveloped_without_database),
    cmocka_unit_test(test_conflict_free_name_passthrough),
    cmocka_unit_test(test_conflict_unique_name),
    cmocka_unit_test(test_conflict_dotted_directory),
    cmocka_unit_test(test_conflict_overwrite_and_skip),
    cmocka_unit_test(test_conflict_in_batch_collision),
    cmocka_unit_test(test_conflict_in_batch_skips_existing),
    cmocka_unit_test(test_char_capacity_counts_the_array),
    cmocka_unit_test(test_resolve_field_accepts_bare_array_name),
    cmocka_unit_test(test_char_field_round_trip),
    cmocka_unit_test(test_char_field_truncates_on_utf8_boundary),
    cmocka_unit_test(test_char_field_shorter_string_zeroes_tail),
    cmocka_unit_test(test_char_field_refuses_numbers),
    cmocka_unit_test(test_unknown_field_still_refused),
  };
  return cmocka_run_group_tests(tests, _setup, _teardown);
}
