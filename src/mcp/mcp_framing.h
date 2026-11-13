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

// message framing shared by every MCP transport: the stdio loop and the
// in-GUI socket server both speak the same two wire formats, the MCP stdio
// default of one JSON object per line, and LSP-style Content-Length framing
// (selected automatically per peer from the first bytes seen)

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/** one peer's parse state; treat as opaque, init with mcp_framer_init() */
typedef struct mcp_framer_t
{
  gboolean framing;      // peer uses Content-Length framing (else newline)
  gboolean known;        // framing decided yet?
  gsize want;            // in framing mode: payload bytes still missing
  GByteArray *buf;
} mcp_framer_t;

void mcp_framer_init(mcp_framer_t *fr);
void mcp_framer_clear(mcp_framer_t *fr);

/** feed wire bytes; on return *consumed says how many were absorbed.
    returns 1 and sets *msg (g_free it) when one complete message is out,
    0 when more bytes are needed, -1 on a protocol error (state reset) */
int mcp_framer_feed(mcp_framer_t *fr, const guint8 *data, gsize len,
                    gsize *consumed, gchar **msg);

/** encode json for the wire in the given framing; g_free the result */
guint8 *mcp_framer_encode(const gchar *json, gsize len, gboolean framing,
                          gsize *wire_len);

G_END_DECLS
