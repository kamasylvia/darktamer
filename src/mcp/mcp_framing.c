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

#include "mcp/mcp_framing.h"

#include <string.h>

void mcp_framer_init(mcp_framer_t *fr)
{
  fr->framing = FALSE;
  fr->known = FALSE;
  fr->want = 0;
  fr->buf = g_byte_array_new();
}

void mcp_framer_clear(mcp_framer_t *fr)
{
  g_byte_array_unref(fr->buf);
  fr->buf = NULL;
}

// how many payload bytes the Content-Length header in buf asks for,
// or -1 when the header line is malformed
static gssize _header_length(const GByteArray *buf)
{
  const gchar *data = (const gchar *)buf->data;
  const gsize len = buf->len;
  const char *colon = memchr(data, ':', MIN(len, 64));
  if(!colon) return 0;   // header line not complete yet
  const gssize clen = g_ascii_strtoll(colon + 1, NULL, 10);
  return clen > 0 ? clen : -1;
}

// once a full header block is buffered, read its length into fr->want and
// drop the header bytes; 0 when still incomplete, -1 on a malformed header
static int _scan_framing_header(mcp_framer_t *fr)
{
  if(fr->want > 0) return 0;
  for(gsize i = 0; i + 1 < fr->buf->len; i++)
  {
    gsize seplen = 0;
    if(fr->buf->data[i] == '\n' && fr->buf->data[i + 1] == '\n')
      seplen = 2;
    else if(fr->buf->data[i] == '\r' && fr->buf->data[i + 1] == '\n'
            && i + 3 < fr->buf->len
            && fr->buf->data[i + 2] == '\r' && fr->buf->data[i + 3] == '\n')
      seplen = 4;
    if(!seplen) continue;

    GByteArray header;
    header.data = fr->buf->data;
    header.len = i;
    const gssize clen = _header_length(&header);
    if(clen <= 0)
    {
      g_byte_array_set_size(fr->buf, 0);
      return -1;
    }
    g_byte_array_remove_range(fr->buf, 0, i + seplen);
    fr->want = (gsize)clen;
    return 0;
  }
  return 0;
}

// callers feed whatever bytes arrived and keep calling until 0: every message
// that has arrived whole comes out here, whether it was complete on arrival or
// the tail of an earlier chunk. pipelined requests must not sit in the buffer
// waiting for bytes that are never coming
int mcp_framer_feed(mcp_framer_t *fr, const guint8 *data, gsize len,
                    gsize *consumed, gchar **msg)
{
  *consumed = 0;
  *msg = NULL;

  for(;;)
  {
    if(fr->known)
    {
      if(!fr->framing)
      {
        guint8 *nl = memchr(fr->buf->data, '\n', fr->buf->len);
        if(nl)
        {
          gsize linelen = nl - fr->buf->data;
          while(linelen > 0 && fr->buf->data[linelen - 1] == '\r') linelen--;
          gchar *line = g_strndup((const gchar *)fr->buf->data, linelen);
          g_byte_array_remove_range(fr->buf, 0, nl - fr->buf->data + 1);
          if(linelen == 0)
          {
            g_free(line);
            continue;   // blank line between messages
          }
          *msg = line;
          return 1;
        }
      }
      else
      {
        if(_scan_framing_header(fr) < 0) return -1;
        if(fr->want > 0 && fr->buf->len >= fr->want)
        {
          *msg = g_strndup((const gchar *)fr->buf->data, fr->want);
          g_byte_array_remove_range(fr->buf, 0, fr->want);
          fr->want = 0;
          return 1;
        }
      }
    }
    else
    {
      // the first line decides the format: Content-Length: means LSP
      // framing, anything else is newline-delimited
      const guint8 *nl = memchr(fr->buf->data, '\n', fr->buf->len);
      if(nl)
      {
        fr->known = TRUE;
        fr->framing = (gsize)(nl - fr->buf->data) >= 15
          && g_ascii_strncasecmp((const gchar *)fr->buf->data,
                                 "Content-Length:", 15) == 0;
        continue;
      }
    }

    // nothing complete yet: absorb the rest of this chunk, and come back to
    // re-examine what is now buffered before asking for more
    if(*consumed >= len) return 0;
    g_byte_array_append(fr->buf, data + *consumed, len - *consumed);
    *consumed = len;
  }
}

guint8 *mcp_framer_encode(const gchar *json, gsize len, gboolean framing,
                          gsize *wire_len)
{
  if(framing)
  {
    gchar *head = g_strdup_printf("Content-Length: %zu\r\n\r\n", len);
    const gsize hlen = strlen(head);
    guint8 *out = g_malloc(hlen + len);
    memcpy(out, head, hlen);
    memcpy(out + hlen, json, len);
    g_free(head);
    *wire_len = hlen + len;
    return out;
  }
  guint8 *out = g_malloc(len + 1);
  memcpy(out, json, len);
  out[len] = '\n';
  *wire_len = len + 1;
  return out;
}
