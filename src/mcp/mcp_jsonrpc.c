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

#include "mcp/mcp_jsonrpc.h"
#include "mcp/mcp_framing.h"
#include "mcp/mcp_tools.h"

#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include "win/getdelim.h" // getline() is not declared by MinGW
#endif

#define SERVER_NAME "darktable-mcp"
#define SERVER_VERSION "0.1.0"
#define PROTOCOL_VERSION "2025-06-18"

// ---------------------------------------------------------------------------
// envelope builders
// ---------------------------------------------------------------------------

JsonNode *mcp_jsonrpc_result_node(JsonNode *id, JsonNode *result)
{
  JsonObject *o = json_object_new();
  json_object_set_string_member(o, "jsonrpc", "2.0");
  if(id) json_object_set_member(o, "id", json_node_copy(id));
  else    json_object_set_null_member(o, "id");
  json_object_set_member(o, "result", result);
  JsonNode *root = json_node_new(JSON_NODE_OBJECT);
  json_node_take_object(root, o);
  return root;
}

JsonNode *mcp_jsonrpc_error_node(JsonNode *id, int code, const char *msg)
{
  JsonObject *err = json_object_new();
  json_object_set_int_member(err, "code", code);
  json_object_set_string_member(err, "message", msg ? msg : "");

  JsonObject *o = json_object_new();
  json_object_set_string_member(o, "jsonrpc", "2.0");
  if(id) json_object_set_member(o, "id", json_node_copy(id));
  else    json_object_set_null_member(o, "id");
  json_object_set_object_member(o, "error", err);

  JsonNode *root = json_node_new(JSON_NODE_OBJECT);
  json_node_take_object(root, o);
  return root;
}

// ---------------------------------------------------------------------------
// method results
// ---------------------------------------------------------------------------

static JsonNode *_empty_object(void)
{
  JsonNode *n = json_node_new(JSON_NODE_OBJECT);
  json_node_take_object(n, json_object_new());
  return n;
}

static JsonNode *_initialize_result(void)
{
  JsonObject *tools = json_object_new();
  json_object_set_boolean_member(tools, "listChanged", FALSE);

  JsonObject *caps = json_object_new();
  json_object_set_object_member(caps, "tools", tools);

  JsonObject *info = json_object_new();
  json_object_set_string_member(info, "name", SERVER_NAME);
  json_object_set_string_member(info, "version", SERVER_VERSION);

  JsonObject *o = json_object_new();
  json_object_set_string_member(o, "protocolVersion", PROTOCOL_VERSION);
  json_object_set_object_member(o, "capabilities", caps);
  json_object_set_object_member(o, "serverInfo", info);

  JsonNode *n = json_node_new(JSON_NODE_OBJECT);
  json_node_take_object(n, o);
  return n;
}

static JsonNode *_tools_list_result(void)
{
  JsonObject *o = json_object_new();
  json_object_set_member(o, "tools", mcp_tools_list_node());
  JsonNode *n = json_node_new(JSON_NODE_OBJECT);
  json_node_take_object(n, o);
  return n;
}

gboolean mcp_jsonrpc_parse_tools_call(JsonObject *req, const char **name,
                                      JsonObject **args)
{
  *name = NULL;
  *args = NULL;
  if(!json_object_has_member(req, "params")) return FALSE;
  JsonNode *pn = json_object_get_member(req, "params");
  if(!JSON_NODE_HOLDS_OBJECT(pn)) return FALSE;
  JsonObject *params = json_node_get_object(pn);

  if(json_object_has_member(params, "name"))
  {
    JsonNode *nn = json_object_get_member(params, "name");
    if(JSON_NODE_HOLDS_VALUE(nn) && json_node_get_value_type(nn) == G_TYPE_STRING)
      *name = json_object_get_string_member(params, "name");
  }
  if(json_object_has_member(params, "arguments"))
  {
    JsonNode *an = json_object_get_member(params, "arguments");
    if(JSON_NODE_HOLDS_OBJECT(an)) *args = json_node_get_object(an);
  }
  return *name != NULL;
}

// ---------------------------------------------------------------------------
// message handling
// ---------------------------------------------------------------------------

JsonNode *mcp_jsonrpc_handle(JsonNode *root, gboolean *shutdown)
{
  *shutdown = FALSE;
  if(!root || !JSON_NODE_HOLDS_OBJECT(root))
    return mcp_jsonrpc_error_node(NULL, MCP_ERR_INVALID_REQUEST,
                                  "not a JSON-RPC object");

  JsonObject *req = json_node_get_object(root);
  const char *method = json_object_has_member(req, "method")
                         ? json_object_get_string_member(req, "method") : NULL;
  JsonNode *id = json_object_has_member(req, "id")
                   ? json_object_get_member(req, "id") : NULL;

  if(!method)
  {
    if(id) return mcp_jsonrpc_error_node(id, MCP_ERR_INVALID_REQUEST,
                                         "missing method");
    return NULL;
  }

  if(!strcmp(method, "initialize"))
    return mcp_jsonrpc_result_node(id, _initialize_result());
  if(!strcmp(method, "tools/list"))
    return mcp_jsonrpc_result_node(id, _tools_list_result());
  if(!strcmp(method, "tools/call"))
  {
    const char *name = NULL;
    JsonObject *args = NULL;
    if(!mcp_jsonrpc_parse_tools_call(req, &name, &args))
      return mcp_jsonrpc_error_node(id, MCP_ERR_INVALID_PARAMS,
                                    "missing tool name");
    gboolean found = FALSE;
    JsonNode *result = mcp_tools_call_node(name, args, &found);
    if(!found)
    {
      if(result) json_node_unref(result);
      return mcp_jsonrpc_error_node(id, MCP_ERR_METHOD_NOT_FOUND, "unknown tool");
    }
    return mcp_jsonrpc_result_node(id, result);
  }
  if(!strcmp(method, "ping"))
    return mcp_jsonrpc_result_node(id, _empty_object());
  if(g_str_has_prefix(method, "notifications/"))
    return NULL;   // notifications get no response
  if(!strcmp(method, "shutdown"))
  {
    *shutdown = TRUE;
    return mcp_jsonrpc_result_node(id, _empty_object());
  }
  if(id)
    return mcp_jsonrpc_error_node(id, MCP_ERR_METHOD_NOT_FOUND, "method not found");
  return NULL;
}

// ---------------------------------------------------------------------------
// stdio read loop
// ---------------------------------------------------------------------------

// serialize a response node to the stream in the peer's framing
static void _write_node(FILE *out, JsonNode *root, gboolean framing)
{
  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, root);
  gsize len = 0;
  gchar *s = json_generator_to_data(gen, &len);
  gsize wlen = 0;
  guint8 *wire = mcp_framer_encode(s, len, framing, &wlen);
  fwrite(wire, 1, wlen, out);
  fflush(out);
  g_free(wire);
  g_free(s);
  g_object_unref(gen);
}

void mcp_jsonrpc_loop(FILE *in, FILE *out)
{
  mcp_framer_t fr;
  mcp_framer_init(&fr);
  guint8 buf[16384];
  gboolean done = FALSE;

  while(!done)
  {
    const size_t n = fread(buf, 1, sizeof(buf), in);
    if(n == 0) break;

    const guint8 *p = buf;
    gsize left = n;
    for(;;)
    {
      gchar *msg = NULL;
      gsize consumed = 0;
      const int r = mcp_framer_feed(&fr, p, left, &consumed, &msg);
      p += consumed;
      left -= consumed;

      if(r < 0)
      {
        JsonNode *resp = mcp_jsonrpc_error_node(NULL, MCP_ERR_PARSE,
                                                "protocol error");
        _write_node(out, resp, TRUE);
        json_node_unref(resp);
        continue;
      }
      if(r == 0) break;

      JsonParser *parser = json_parser_new();
      GError *e = NULL;
      JsonNode *resp = NULL;
      if(!json_parser_load_from_data(parser, msg, -1, &e))
        resp = mcp_jsonrpc_error_node(NULL, MCP_ERR_PARSE,
                                      e ? e->message : "parse error");
      else
      {
        gboolean shut = FALSE;
        resp = mcp_jsonrpc_handle(json_parser_get_root(parser), &shut);
        done = shut;
      }
      if(resp) _write_node(out, resp, fr.framing);
      json_node_unref(resp);
      g_object_unref(parser);
      g_free(msg);
      if(done) break;
      // pipelined bytes: keep draining the buffer with nothing new fed in
      left = 0;
    }
  }

  mcp_framer_clear(&fr);
}
