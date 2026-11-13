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

#include "mcp/mcp_gui_server.h"
#include "mcp/mcp_framing.h"
#include "mcp/mcp_jsonrpc.h"
#include "mcp/mcp_tools.h"
#include "mcp/dt_bridge.h"

#include "common/darktable.h"
#include "control/conf.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <unistd.h>

#ifdef G_OS_UNIX
#include <gio/gunixsocketaddress.h>
#endif

static void _mcp_gui_close_connection(void);
static void _handle_message(const gchar *msg);

// one connection at a time, and one request at a time: the bridge below is
// strictly sequential, matching the headless server's posture. requests are
// parsed on the GUI thread (the socket sources run on the default context,
// which is the GTK loop), then either answered right there -- the tools that
// drive the GUI or the open image -- or handed as a JSON string to the single
// worker thread. the worker's reply comes back through an idle callback, so
// every byte in and out of the socket happens on the GUI thread and no
// locking around the pending buffer is needed

typedef struct _mcp_gui_t
{
  gboolean running;
  gchar *path;
  GSocketService *service;
  GSocketConnection *conn;
  GSocket *sock;
  mcp_framer_t framer;
  GSource *read_source;
  GSource *write_source;
  GByteArray *pending;          // unwritten bytes, GUI thread only
  gboolean busy;                // worker executing a request

  GMutex job_lock;
  GCond job_cond;
  gchar *job;                   // serialized request waiting for the worker
  gboolean quit_worker;
  GThread *worker;
} _mcp_gui_t;

static _mcp_gui_t S;

// the fall-back socket path, with no conf access: the --bridge connector
// calls this before any darktable init, so it must stay pure
gchar *dt_mcp_gui_server_default_path(void)
{
#ifdef G_OS_UNIX
  const gchar *runtime = g_get_user_runtime_dir();
  if(runtime && *runtime) return g_build_filename(runtime, "darktable-mcp.sock", NULL);
#endif
  return g_strdup_printf("%s%cdarktable-mcp-%u.sock", g_get_tmp_dir(),
                         G_DIR_SEPARATOR, (guint)getuid());
}

// ---------------------------------------------------------------------------
// socket output (GUI thread only)
// ---------------------------------------------------------------------------

static gboolean _on_writable(gpointer user_data)
{
  (void)user_data;
  if(!S.sock)
  {
    S.write_source = NULL;
    return FALSE;
  }
  while(S.pending->len > 0)
  {
    const gssize n = g_socket_send(S.sock, (const gchar *)S.pending->data,
                                   S.pending->len, NULL, NULL);
    if(n <= 0) break;   // would block (or gone): the rest waits for the next wakeup
    g_byte_array_remove_range(S.pending, 0, (gsize)n);
  }
  if(S.pending->len == 0)
  {
    S.write_source = NULL;
    return FALSE;   // nothing left: retire the source until the next write
  }
  return TRUE;
}

static void _socket_write(const guint8 *data, gsize len)
{
  if(!S.sock) return;
  if(!S.pending) S.pending = g_byte_array_new();
  g_byte_array_append(S.pending, data, len);
  if(!S.write_source)
  {
    S.write_source = g_socket_create_source(S.sock, G_IO_OUT, NULL);
    g_source_set_callback(S.write_source, _on_writable, NULL, NULL);
    g_source_attach(S.write_source, NULL);
    g_source_unref(S.write_source);
  }
}

// serialize a response node on the wire
static void _send_node(JsonNode *root, gboolean framing)
{
  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, root);
  gsize len = 0;
  gchar *s = json_generator_to_data(gen, &len);
  gsize wlen = 0;
  guint8 *wire = mcp_framer_encode(s, len, framing, &wlen);
  _socket_write(wire, wlen);
  g_free(wire);
  g_free(s);
  g_object_unref(gen);
}

// ---------------------------------------------------------------------------
// the worker thread: runs everything the GUI thread must not
// ---------------------------------------------------------------------------

// extract whatever whole messages are still sitting in the framer after the
// worker's reply unblocked the bridge: the read source only wakes on new
// bytes, so pipelined requests would otherwise wait forever
static void _drain_framer(void)
{
  for(;;)
  {
    gchar *msg = NULL;
    gsize consumed = 0;
    const int r = mcp_framer_feed(&S.framer, NULL, 0, &consumed, &msg);
    if(r <= 0) return;
    _handle_message(msg);
    g_free(msg);
    if(S.busy || !S.conn) return;
  }
}

// delivered on the GUI thread: write the worker's finished reply
static gboolean _deliver_reply(gpointer data)
{
  gchar *json = (gchar *)data;
  JsonParser *p = json_parser_new();
  JsonNode *root = NULL;
  if(json_parser_load_from_data(p, json, -1, NULL))
  {
    root = json_parser_get_root(p);
    _send_node(root, S.framer.framing);
  }
  g_object_unref(p);
  g_free(json);
  S.busy = FALSE;   // the bridge may take the next request
  if(S.conn) _drain_framer();
  return FALSE;     // one-shot idle
}

// worker body: parse, run the tool, marshal the reply back
static gpointer _worker_thread(gpointer data)
{
  (void)data;
  g_mutex_lock(&S.job_lock);
  for(;;)
  {
    while(!S.job && !S.quit_worker)
      g_cond_wait(&S.job_cond, &S.job_lock);
    if(S.quit_worker) break;

    gchar *job = S.job;
    S.job = NULL;
    g_mutex_unlock(&S.job_lock);

    JsonParser *p = json_parser_new();
    JsonNode *reply = NULL;
    if(json_parser_load_from_data(p, job, -1, NULL))
    {
      JsonNode *root = json_parser_get_root(p);
      if(root && JSON_NODE_HOLDS_OBJECT(root))
      {
        const char *name = NULL;
        JsonObject *args = NULL;
        mcp_jsonrpc_parse_tools_call(json_node_get_object(root), &name, &args);
        gboolean found = FALSE;
        JsonNode *result = mcp_tools_call_node(name, args, &found);
        if(!found)
        {
          if(result) json_node_unref(result);
          reply = mcp_jsonrpc_error_node(NULL, MCP_ERR_METHOD_NOT_FOUND,
                                         "unknown tool");
        }
        else
        {
          // rebuild the envelope from the request id
          JsonObject *req = json_node_get_object(root);
          JsonNode *id = json_object_has_member(req, "id")
                           ? json_object_get_member(req, "id") : NULL;
          reply = mcp_jsonrpc_result_node(id, result);
        }
      }
    }
    if(!reply)
      reply = mcp_jsonrpc_error_node(NULL, MCP_ERR_INVALID_REQUEST,
                                     "bad request to worker");
    g_object_unref(p);

    // serialize here; only the byte write marshals back to the GUI thread
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, reply);
    gsize len = 0;
    gchar *s = json_generator_to_data(gen, &len);
    g_object_unref(gen);
    json_node_unref(reply);

    g_main_context_invoke(NULL, _deliver_reply, g_strdup(s));
    g_free(s);

    g_mutex_lock(&S.job_lock);
  }
  g_mutex_unlock(&S.job_lock);
  return NULL;
}

// enqueue a request for the worker; called on the GUI thread
static void _to_worker(JsonNode *root)
{
  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, root);
  gsize len = 0;
  gchar *s = json_generator_to_data(gen, &len);
  g_object_unref(gen);

  g_mutex_lock(&S.job_lock);
  S.job = s;   // ownership passes to the worker
  g_cond_signal(&S.job_cond);
  g_mutex_unlock(&S.job_lock);
  S.busy = TRUE;
}

// ---------------------------------------------------------------------------
// request handling (GUI thread)
// ---------------------------------------------------------------------------

static void _handle_message(const gchar *msg)
{
  JsonParser *parser = json_parser_new();
  GError *e = NULL;
  if(!json_parser_load_from_data(parser, msg, -1, &e))
  {
    JsonNode *resp = mcp_jsonrpc_error_node(NULL, MCP_ERR_PARSE,
                                            e ? e->message : "parse error");
    _send_node(resp, S.framer.framing);
    json_node_unref(resp);
    g_object_unref(parser);
    if(e) g_error_free(e);
    return;
  }

  JsonNode *root = json_parser_get_root(parser);
  JsonObject *req = (root && JSON_NODE_HOLDS_OBJECT(root))
                      ? json_node_get_object(root) : NULL;
  const char *method = req && json_object_has_member(req, "method")
                         ? json_object_get_string_member(req, "method") : NULL;

  // tools/call gets the routing treatment; the protocol methods run inline
  const char *name = NULL;
  JsonObject *args = NULL;
  if(method && !strcmp(method, "tools/call")
     && mcp_jsonrpc_parse_tools_call(req, &name, &args))
  {
    if(mcp_tools_targets_open(name, args))
    {
      // on the GUI thread already: run it here, answer right away
      gboolean found = FALSE;
      JsonNode *result = mcp_tools_call_node(name, args, &found);
      JsonNode *id = json_object_has_member(req, "id")
                       ? json_object_get_member(req, "id") : NULL;
      if(!found)
      {
        if(result) json_node_unref(result);
        JsonNode *resp = mcp_jsonrpc_error_node(id, MCP_ERR_METHOD_NOT_FOUND,
                                                "unknown tool");
        _send_node(resp, S.framer.framing);
        json_node_unref(resp);
      }
      else
      {
        JsonNode *resp = mcp_jsonrpc_result_node(id, result);
        _send_node(resp, S.framer.framing);
        json_node_unref(resp);
      }
    }
    else
    {
      // off-thread; the read source pauses on S.busy until the reply lands
      _to_worker(root);
    }
    g_object_unref(parser);
    return;
  }

  gboolean shut = FALSE;
  JsonNode *resp = mcp_jsonrpc_handle(root, &shut);
  if(resp)
  {
    _send_node(resp, S.framer.framing);
    json_node_unref(resp);
  }
  g_object_unref(parser);

  if(shut) _mcp_gui_close_connection();
}

// ---------------------------------------------------------------------------
// socket reading (GUI thread)
// ---------------------------------------------------------------------------

static gboolean _on_readable(GSocket *sock, GIOCondition cond, gpointer user)
{
  (void)cond;
  (void)user;
  if(!S.running) { S.read_source = NULL; return FALSE; }

  if(S.busy) return TRUE;   // worker holds the bridge; leave bytes buffered

  guint8 buf[16384];
  gssize n;
  while((n = g_socket_receive(sock, (gchar *)buf, sizeof(buf), NULL, NULL)) > 0)
  {
    const guint8 *p = buf;
    gsize left = (gsize)n;
    for(;;)
    {
      gchar *msg = NULL;
      gsize consumed = 0;
      const int r = mcp_framer_feed(&S.framer, p, left, &consumed, &msg);
      p += consumed;
      left -= consumed;
      if(r < 0)
      {
        JsonNode *resp = mcp_jsonrpc_error_node(NULL, MCP_ERR_PARSE,
                                                "protocol error");
        _send_node(resp, S.framer.framing);
        json_node_unref(resp);
        continue;
      }
      if(r == 0) break;
      _handle_message(msg);
      g_free(msg);
      if(S.busy) break;   // pipelined bytes stay buffered until the reply lands
      left = 0;           // keep draining the buffer with nothing new fed in
    }
  }
  if(n == 0)   // peer closed; n < 0 with WOULD_BLOCK is the normal dry read
    _mcp_gui_close_connection();
  return S.running && S.conn ? TRUE : FALSE;
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

static gboolean _incoming(GSocketService *service, GSocketConnection *conn,
                          GObject *source, gpointer user)
{
  (void)service;
  (void)source;
  (void)user;

  if(S.conn)
  {
    // already serving someone: refuse politely
    GSocket *sock = g_socket_connection_get_socket(conn);
    static const char busy[] = "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":"
      "{\"code\":-32000,\"message\":\"another MCP client is connected\"}}\n";
    g_socket_send(sock, busy, sizeof(busy) - 1, NULL, NULL);
    g_socket_close(sock, NULL);
    return TRUE;
  }

  S.conn = g_object_ref(conn);
  S.sock = g_object_ref(g_socket_connection_get_socket(conn));
  g_socket_set_blocking(S.sock, FALSE);
  mcp_framer_init(&S.framer);

  S.read_source = g_socket_create_source(S.sock, G_IO_IN, NULL);
  g_source_set_callback(S.read_source, (GSourceFunc)_on_readable, NULL, NULL);
  g_source_attach(S.read_source, NULL);
  g_source_unref(S.read_source);

  dt_print(DT_DEBUG_CONTROL, "[mcp] client connected");
  return TRUE;
}

static void _mcp_gui_close_connection(void)
{
  if(S.read_source)
  {
    g_source_destroy(S.read_source);
    S.read_source = NULL;
  }
  if(S.write_source)
  {
    g_source_destroy(S.write_source);
    S.write_source = NULL;
  }
  if(S.conn)
  {
    g_socket_close(S.sock, NULL);
    g_object_unref(S.conn);
    g_object_unref(S.sock);
    S.conn = NULL;
    S.sock = NULL;
    mcp_framer_clear(&S.framer);
  }
  if(S.pending) g_byte_array_set_size(S.pending, 0);
  dt_print(DT_DEBUG_CONTROL, "[mcp] client disconnected");
}

void dt_mcp_gui_server_start(const char *socket_override)
{
  if(!dt_conf_get_bool("plugins/mcp/gui_server"))
  {
    dt_print(DT_DEBUG_CONTROL, "[mcp] GUI server disabled (plugins/mcp/gui_server off)");
    return;
  }
#ifdef G_OS_UNIX
  memset(&S, 0, sizeof(S));
  S.pending = g_byte_array_new();
  g_mutex_init(&S.job_lock);
  g_cond_init(&S.job_cond);

  gchar *conf_path = dt_conf_get_string("plugins/mcp/gui_socket");
  S.path = g_strdup(socket_override ? socket_override
                                    : (conf_path && *conf_path ? conf_path
                                                               : NULL));
  if(!S.path) S.path = dt_mcp_gui_server_default_path();
  g_free(conf_path);
  g_unlink(S.path);   // a stale socket from a crashed instance is not alive

  GError *err = NULL;
  S.service = g_socket_service_new();
  GSocketAddress *addr = g_unix_socket_address_new(S.path);
  if(!g_socket_listener_add_address(G_SOCKET_LISTENER(S.service), addr,
                                    G_SOCKET_TYPE_STREAM,
                                    G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &err))
  {
    fprintf(stderr, "darktable: MCP GUI server not started: %s\n",
            err ? err->message : "?");
    if(err) g_error_free(err);
    g_object_unref(addr);
    g_object_unref(S.service);
    g_byte_array_unref(S.pending);
    g_free(S.path);
    memset(&S, 0, sizeof(S));
    return;
  }
  g_object_unref(addr);
  g_chmod(S.path, 0600);   // same-user clients only

  g_signal_connect(S.service, "incoming", G_CALLBACK(_incoming), NULL);
  g_socket_service_start(S.service);

  S.worker = g_thread_new("darktable-mcp", _worker_thread, NULL);
  S.running = TRUE;
  dt_bridge_set_read_only(dt_conf_get_bool("plugins/mcp/read_only"));
  dt_print(DT_DEBUG_CONTROL, "[mcp] GUI server listening on %s", S.path);
#else
  (void)socket_override;
  fprintf(stderr, "darktable: the MCP GUI server needs unix sockets\n");
#endif
}

void dt_mcp_gui_server_stop(void)
{
#ifdef G_OS_UNIX
  if(!S.running) return;
  S.running = FALSE;
  g_socket_service_stop(S.service);
  _mcp_gui_close_connection();
  g_mutex_lock(&S.job_lock);
  S.quit_worker = TRUE;
  g_cond_signal(&S.job_cond);
  g_mutex_unlock(&S.job_lock);
  g_thread_join(S.worker);
  g_mutex_clear(&S.job_lock);
  g_cond_clear(&S.job_cond);
  if(S.pending) g_byte_array_unref(S.pending);
  g_unlink(S.path);
  g_free(S.path);
  memset(&S, 0, sizeof(S));
#endif
}
