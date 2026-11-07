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

static gboolean _prewarm_poll(gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_cache_prewarm_t *d = (dt_lib_cache_prewarm_t *)self->data;

  if(dt_control_prewarm_cache_running()) return TRUE; // keep polling

  gtk_widget_set_sensitive(GTK_WIDGET(d->button), TRUE);
  d->poll_id = 0;
  return FALSE; // stop polling
}

static void _button_clicked(GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_cache_prewarm_t *d = (dt_lib_cache_prewarm_t *)self->data;

  if(dt_control_prewarm_cache_running())
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

  gtk_widget_set_sensitive(GTK_WIDGET(d->button), FALSE);
  if(d->poll_id == 0)
    d->poll_id = g_timeout_add_seconds(1, _prewarm_poll, self);

  dt_control_prewarm_cache_start(imgids);
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
