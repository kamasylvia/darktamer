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

#pragma once

#include <stdio.h>
#include <json-glib/json-glib.h>

// JSON-RPC error codes shared by every transport
#define MCP_ERR_PARSE           -32700
#define MCP_ERR_INVALID_REQUEST -32600
#define MCP_ERR_METHOD_NOT_FOUND -32601
#define MCP_ERR_INVALID_PARAMS  -32602

/** blocking MCP (JSON-RPC 2.0 over stdio) event loop: reads requests from `in`,
    writes responses to `out`, returns on EOF or a `shutdown` request */
void mcp_jsonrpc_loop(FILE *in, FILE *out);

/** handle one parsed JSON-RPC message object; returns an owned response node
    (free with json_node_unref), or NULL when nothing is to be sent
    (notifications). sets *shutdown on a `shutdown` request */
JsonNode *mcp_jsonrpc_handle(JsonNode *root, gboolean *shutdown);

/** JSON-RPC envelope builders; ownership of `result` passes to the returned
    node (id, if given, is copied) */
JsonNode *mcp_jsonrpc_result_node(JsonNode *id, JsonNode *result);
JsonNode *mcp_jsonrpc_error_node(JsonNode *id, int code, const char *msg);

/** parse a `tools/call` request object into its tool name and arguments
    object (either may stay NULL); FALSE when the request is malformed */
gboolean mcp_jsonrpc_parse_tools_call(JsonObject *req, const char **name,
                                      JsonObject **args);
