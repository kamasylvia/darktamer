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

// the in-GUI MCP server: a unix-socket listener on the default glib main
// context that answers the same JSON-RPC protocol as the headless
// darktable-mcp binary. requests whose target is the image the darkroom is
// showing run on the GUI thread and update the canvas live; everything else
// runs on one worker thread. darktable-mcp --bridge shuttles bytes between
// this socket and an MCP client's stdio.

#pragma once

#include <glib.h>

/** bind the listener and start serving. a no-op unless the conf key
    plugins/mcp/gui_server is set (the --mcp flag sets it). fails silently
    into stderr when the socket cannot be bound, e.g. for a second instance.
    socket_override replaces the conf/default path when non-NULL (--mcp-socket) */
void dt_mcp_gui_server_start(const char *socket_override);

/** tear everything down; safe to call when never started */
void dt_mcp_gui_server_stop(void);

/** absolute path of the darktable-mcp connector beside this binary
    (g_free the result); falls back to the bare command for PATH lookup */
gchar *dt_mcp_gui_bridge_path(void);

/** the standard mcpServers client block for connecting to this instance,
    with the current socket and bridge path filled in (g_free the result);
    shown in the AI preferences tab and written to mcp-client-config.json */
gchar *dt_mcp_gui_client_config_json(void);

/** the fall-back socket path, $XDG_RUNTIME_DIR/darktable-mcp.sock or a tmpdir
    equivalent (g_free the result). conf-free, so the --bridge connector can
    call it before any darktable init; the server start itself prefers the
    plugins/mcp/gui_socket conf key over it */
gchar *dt_mcp_gui_server_default_path(void);
