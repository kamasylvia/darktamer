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

#include "common/darktable.h"
#include "common/collection.h"
#include "common/mipmap_cache.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

typedef struct dt_lib_cache_prewarm_t
{
  GtkButton *button;
  guint poll_id;
} dt_lib_cache_prewarm_t;

// TRUE while the prewarm job is queued or running; polled by the gui to
// re-enable the button when the job ends
static gboolean _prewarm_running = FALSE;

const char *name(dt_lib_module_t *self)
{
  return _("cache prewarm");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_LIGHTTABLE;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position(const dt_lib_module_t *self)
{
  return 50; // below the collections module
}

typedef struct _prewarm_params_t
{
  GList *imgids; // GINT_TO_POINTER(imgid) list, owned by the job
} _prewarm_params_t;

static void _prewarm_params_free(void *params)
{
  _prewarm_params_t *p = (_prewarm_params_t *)params;
  g_list_free(p->imgids);
  g_free(p);
}

static int32_t _prewarm_job_run(dt_job_t *job)
{
  _prewarm_params_t *params = (_prewarm_params_t *)dt_control_job_get_params(job);
  const guint count = g_list_length(params->imgids);
  double last_update = 0.0;
  int done = 0;

  for(GList *l = params->imgids; l; l = g_list_next(l))
  {
    if(dt_control_job_get_state(job) == DT_JOB_STATE_CANCELLED) break;

    const dt_imgid_t imgid = GPOINTER_TO_INT(l->data);

    // generate the biggest grid level, then write it to the disk cache
    // and drop it from memory immediately: the point of this job is to
    // fill the disk backend, not to hold frames in RAM (a CIRAW full
    // slot is ~1GB). smaller mips are generated from the disk jpg
    // without another full decode
    dt_mipmap_buffer_t buf;
    dt_mipmap_cache_get(&buf, imgid, DT_MIPMAP_8, DT_MIPMAP_BLOCKING, 'r');
    dt_mipmap_cache_release(&buf);
    dt_mipmap_cache_evict_at_size(imgid, DT_MIPMAP_8);

    done++;
    char msg[80];
    snprintf(msg, sizeof(msg), _("prewarming thumbnails (%d / %u)"), done, count);
    dt_control_job_set_progress_message(job, msg);

    const double curr_time = dt_get_wtime();
    if(curr_time > last_update + 0.5)
    {
      dt_control_job_set_progress(job, CLAMP((double)done / (double)count, 0.0, 1.0));
      last_update = curr_time;
    }
  }

  _prewarm_running = FALSE;
  dt_print(DT_DEBUG_ALWAYS, "[cache prewarm] finished, %d thumbnails ensured", done);
  return 0;
}

static gboolean _prewarm_poll(gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_cache_prewarm_t *d = (dt_lib_cache_prewarm_t *)self->data;

  if(_prewarm_running) return TRUE; // keep polling

  gtk_widget_set_sensitive(GTK_WIDGET(d->button), TRUE);
  d->poll_id = 0;
  return FALSE; // stop polling
}

static void _button_clicked(GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_cache_prewarm_t *d = (dt_lib_cache_prewarm_t *)self->data;

  if(_prewarm_running)
  {
    dt_control_log(_("thumbnail cache prewarm already running"));
    return;
  }

  // the current collection, most recently imported last so it finishes first
  GList *imgids = dt_collection_get_all(darktable.collection, -1);
  if(imgids == NULL)
  {
    dt_control_log(_("nothing to prewarm, the collection is empty"));
    return;
  }

  _prewarm_params_t *params = g_malloc0(sizeof(_prewarm_params_t));
  params->imgids = imgids;

  dt_job_t *job = dt_control_job_create(&_prewarm_job_run, _("prewarm thumbnail cache"));
  if(job == NULL)
  {
    _prewarm_params_free(params);
    return;
  }
  dt_control_job_set_params(job, params, _prewarm_params_free);

  _prewarm_running = TRUE;
  gtk_widget_set_sensitive(GTK_WIDGET(d->button), FALSE);
  if(d->poll_id == 0)
    d->poll_id = g_timeout_add_seconds(1, _prewarm_poll, self);

  // without this the job has no progress object and the gui never shows
  // a bar for it; cancellable gives the user a cancel button in the
  // background jobs panel
  dt_control_job_add_progress(job, _("prewarming thumbnails"), TRUE);
  dt_control_add_job(DT_JOB_QUEUE_USER_BG, job);
}

void init(dt_lib_module_t *self)
{
  self->data = g_malloc0(sizeof(dt_lib_cache_prewarm_t));
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_cache_prewarm_t *d = (dt_lib_cache_prewarm_t *)g_malloc0(sizeof(dt_lib_cache_prewarm_t));
  self->data = d;
  self->widget = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));

  GtkWidget *label = gtk_label_new(_("decode thumbnails for the current\ncollection into the disk cache"));
  gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
  gtk_widget_set_halign(label, GTK_ALIGN_CENTER);

  d->button = GTK_BUTTON(gtk_button_new_with_label(_("prewarm cache")));
  g_signal_connect(G_OBJECT(d->button), "clicked", G_CALLBACK(_button_clicked), self);

  gtk_box_pack_start(GTK_BOX(self->widget), label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->button), FALSE, FALSE, 0);

  gtk_widget_show_all(self->widget);
}

void gui_cleanup(dt_lib_module_t *self)
{
  if(self->data)
  {
    dt_lib_cache_prewarm_t *d = (dt_lib_cache_prewarm_t *)self->data;
    if(d->poll_id) g_source_remove(d->poll_id);
    g_free(d);
    self->data = NULL;
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
