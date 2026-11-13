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

#include "gui/preferences_mcp.h"
#include "mcp/mcp_gui_server.h"

#include "common/darktable.h"
#include "control/conf.h"
#include "develop/imageop.h"
#include "gui/gtk.h"
#include "gui/preferences.h"

static GtkWidget *_json_view = NULL;

// push the current socket + bridge path into the copyable block
static void _refresh_json(void)
{
  if(!_json_view) return;
  gchar *json = dt_mcp_gui_client_config_json();
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(_json_view)),
                           json, -1);
  g_free(json);
}

// runs after the string helper's own handler has written the conf
static void _socket_changed(GtkWidget *entry, gpointer user)
{
  (void)entry;
  (void)user;
  _refresh_json();
}

static void _copy_json(GtkWidget *widget, gpointer user)
{
  (void)widget;
  (void)user;
  GtkTextIter s, e;
  GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(_json_view));
  gtk_text_buffer_get_bounds(buf, &s, &e);
  gchar *text = gtk_text_buffer_get_text(buf, &s, &e, FALSE);
  gtk_clipboard_set_text(gtk_clipboard_get_default(gdk_display_get_default()),
                         text, -1);
  g_free(text);
}

// a bold section caption spanning both grid columns
static void _section(GtkWidget *grid, int *line, const char *title)
{
  GtkWidget *lbox = dt_gui_hbox(gtk_label_new(title));
  gtk_widget_set_name(lbox, "pref_section");
  gtk_grid_attach(GTK_GRID(grid), lbox, 0, (*line)++, 2, 1);
}

void init_tab_mcp(GtkWidget *dialog, GtkWidget *stack)
{
  (void)dialog;
  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(3));
  gtk_grid_set_column_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(10));
  gtk_container_add(GTK_CONTAINER(scroll), grid);
  gtk_widget_set_vexpand(grid, TRUE);

  int line = 0;
  _section(grid, &line, _("agent server (MCP)"));

  // the helpers carry label, tooltip and double-click reset from the
  // confgen declaration, and write the conf directly; the server watches
  // the keys on a slow poll, so changes apply while the dialog is open
  // the returned widget is not needed: the helper owns its callbacks
  (void)dt_gui_preferences_bool(GTK_GRID(grid),
                                "plugins/mcp/gui_server", 0, line, FALSE);
  line++;
  GtkWidget *socket = dt_gui_preferences_string(GTK_GRID(grid),
                                                "plugins/mcp/gui_socket", 0, line);
  line++;
  // refresh the client block whenever the socket moves
  g_signal_connect(socket, "changed", G_CALLBACK(_socket_changed), NULL);
  (void)dt_gui_preferences_bool(GTK_GRID(grid),
                                "plugins/mcp/read_only", 0, line, FALSE);
  line++;

  // the client configuration: whatever an MCP client needs, copyable as-is
  GtkWidget *jsonlabel = dt_ui_label_new(_("client configuration (copy into your MCP client)"));
  gtk_widget_set_halign(jsonlabel, GTK_ALIGN_START);
  GtkWidget *jsonlabelev = gtk_event_box_new();
  gtk_container_add(GTK_CONTAINER(jsonlabelev), jsonlabel);
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(jsonlabelev), FALSE);
  gtk_widget_set_tooltip_text(jsonlabelev,
                              _("the same block is written to"
                                " mcp-client-config.json in the config folder"
                                " whenever the server starts"));
  gtk_grid_attach(GTK_GRID(grid), jsonlabelev, 0, line, 1, 1);

  GtkWidget *copy = gtk_button_new_with_label(_("copy to clipboard"));
  gtk_widget_set_halign(copy, GTK_ALIGN_END);
  gtk_widget_set_tooltip_text(copy, _("copy the configuration to the clipboard"));
  g_signal_connect(copy, "clicked", G_CALLBACK(_copy_json), NULL);
  gtk_grid_attach(GTK_GRID(grid), copy, 1, line++, 1, 1);

  _json_view = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(_json_view), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(_json_view), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(_json_view), TRUE);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(_json_view), DT_PIXEL_APPLY_DPI(8));
  gtk_text_view_set_right_margin(GTK_TEXT_VIEW(_json_view), DT_PIXEL_APPLY_DPI(8));
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(_json_view), GTK_WRAP_WORD_CHAR);
  gtk_widget_set_size_request(_json_view, -1, DT_PIXEL_APPLY_DPI(150));
  gtk_widget_set_hexpand(_json_view, TRUE);
  _refresh_json();
  gtk_grid_attach(GTK_GRID(grid), _json_view, 0, line++, 2, 1);

  GtkWidget *hint = gtk_label_new(_("the server runs while the toggle above is on;"
                                   " changes apply immediately, no restart needed"));
  gtk_widget_set_halign(hint, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), hint, 0, line++, 2, 1);

  gtk_widget_show_all(scroll);
  gtk_stack_add_titled(GTK_STACK(stack), scroll, "AI", _("AI"));
}
