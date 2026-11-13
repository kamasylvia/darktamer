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
#include "common/file_location.h"
#include "mcp/dt_bridge.h"
#include "mcp/mcp_gui_server.h"
#include "mcp/mcp_jsonrpc.h"
#include "mcp/mcp_tools.h"

#include <glib.h>
#include <glib/gi18n.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef G_OS_UNIX
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <poll.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#endif

#ifdef _WIN32
#include "win/main_wrapper.h" // provides wmain() for the -municode CRT entry
#endif

static gboolean _has_flag(char **argv, int argc, const char *flag)
{
  for(int i = 0; i < argc; i++)
    if(!g_strcmp0(argv[i], flag)) return TRUE;
  return FALSE;
}

static gboolean _has_conf(char **argv, int argc, const char *key_prefix)
{
  for(int i = 0; i + 1 < argc; i++)
    if(!g_strcmp0(argv[i], "--conf")
       && g_str_has_prefix(argv[i + 1], key_prefix))
      return TRUE;
  return FALSE;
}

#ifdef G_OS_UNIX
// write everything or fail
static gboolean _write_all(int fd, const guint8 *buf, ssize_t n)
{
  while(n > 0)
  {
    const ssize_t w = write(fd, buf, n);
    if(w < 0)
    {
      if(errno == EINTR) continue;
      return FALSE;
    }
    buf += w;
    n -= w;
  }
  return TRUE;
}

// --bridge: pump bytes between an MCP client's stdio and a running GUI's
// socket. framing passes through untouched; each end negotiates it itself.
// stdin EOF shuts the write side down so the GUI's pending replies still
// arrive before the connector exits
static int _bridge_run(const char *path)
{
  GError *err = NULL;
  GSocketClient *client = g_socket_client_new();
  GSocketAddress *addr = g_unix_socket_address_new(path);
  GSocketConnection *conn =
    g_socket_client_connect(client, (GSocketConnectable *)addr, NULL, &err);
  g_object_unref(addr);
  g_object_unref(client);
  if(!conn)
  {
    fprintf(stderr, "darktable-mcp: cannot connect to %s: %s\n"
                    " (is the GUI running with --mcp?)\n",
            path, err ? err->message : "?");
    if(err) g_error_free(err);
    return 1;
  }
  const int sfd = g_socket_get_fd(g_socket_connection_get_socket(conn));
  // non-blocking, so the keepalive probe below can read without hanging
  fcntl(sfd, F_SETFL, fcntl(sfd, F_GETFL, 0) | O_NONBLOCK);

  guint8 buf[16384];
  gboolean stdin_open = TRUE;
  gboolean alive = TRUE;
  while(alive)
  {
    struct pollfd pfd[2] = { { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 },
                             { .fd = sfd, .events = POLLIN, .revents = 0 } };
    // a timeout, since poll() has been seen sleeping through a unix-socket
    // peer close on macOS; the probe below then ends the loop
    const int r = poll(pfd, stdin_open ? 2 : 1, 1000);
    if(r < 0)
    {
      if(errno == EINTR) continue;
      break;
    }
    if(stdin_open && (pfd[0].revents & (POLLIN | POLLHUP)))
    {
      const ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
      if(n <= 0)
      {
        stdin_open = FALSE;
        shutdown(sfd, SHUT_WR);
      }
      else if(!_write_all(sfd, buf, n))
        break;
    }
    if((pfd[1].revents & (POLLIN | POLLHUP | POLLERR)) || r == 0)
    {
      const ssize_t n = read(sfd, buf, sizeof(buf));
      if(n <= 0)
      {
        if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        alive = FALSE;
      }
      else if(!_write_all(STDOUT_FILENO, buf, n))
        alive = FALSE;
    }
  }
  g_object_unref(conn);
  return 0;
}
#endif

int main(int argc, char **argv)
{
  dt_loc_init(NULL, NULL, NULL, NULL, NULL, NULL);

  // --bridge: connector to a running GUI, no darktable init at all. a path
  // from --socket wins over the default one the GUI advertises
  if(_has_flag(argv, argc, "--bridge"))
  {
#ifdef G_OS_UNIX
    const char *socket_path = NULL;
    for(int i = 1; i + 1 < argc; i++)
      if(!g_strcmp0(argv[i], "--socket")) socket_path = argv[i + 1];
    gchar *def = NULL;
    if(!socket_path) def = dt_mcp_gui_server_default_path();
    const int rc = _bridge_run(socket_path ? socket_path : def);
    g_free(def);
    return rc;
#else
    fprintf(stderr, "darktable-mcp: --bridge needs unix sockets\n");
    return 1;
#endif
  }

  char localedir[PATH_MAX] = { 0 };
  dt_loc_get_localedir(localedir, sizeof(localedir));
  bindtextdomain(GETTEXT_PACKAGE, localedir);
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
  textdomain(GETTEXT_PACKAGE);

  // everything after "--core" is forwarded verbatim to dt_init, so all darktable
  // core options (--configdir, --cachedir, --library, --conf, -d, ...) work as
  // they do for darktable itself
  int core_start = argc;
  for(int i = 1; i < argc; i++)
    if(!g_strcmp0(argv[i], "--core")) { core_start = i + 1; break; }
  const int n_core = argc - core_start;
  char **core = (n_core > 0) ? &argv[core_start] : NULL;

  // --read-only is ours, and everything past --core goes to dt_init, which
  // answers an option it does not know with its whole usage text
  if(_has_flag(core, n_core, "--read-only"))
  {
    fprintf(stderr, "darktable-mcp: --read-only belongs before --core;"
                    " usage: darktable-mcp [--read-only]"
                    " [--core <darktable options>]\n");
    return 1;
  }
  const gboolean read_only = _has_flag(argv, core_start, "--read-only");

  // inject defaults only when the user named neither: both mean "touch
  // nothing". naming a real library opts into persistence, so their own
  // preference applies there
  GPtrArray *m = g_ptr_array_new();
  g_ptr_array_add(m, (gpointer)"darktable-mcp");
  const gboolean adhoc = !_has_flag(core, n_core, "--library")
                      && !_has_flag(core, n_core, "--configdir");
  if(adhoc)
  {
    g_ptr_array_add(m, (gpointer)"--library");
    g_ptr_array_add(m, (gpointer)":memory:");
  }
  if(adhoc && !_has_conf(core, n_core, "write_sidecar_files="))
  {
    g_ptr_array_add(m, (gpointer)"--conf");
    g_ptr_array_add(m, (gpointer)"write_sidecar_files=never");
  }
  for(int i = 0; i < n_core; i++) g_ptr_array_add(m, core[i]);
  const int m_argc = m->len;
  char **m_arg = (char **)m->pdata;

  // keep the protocol channel clean: libdarktable prints to stdout in places, so
  // reserve the real stdout for JSON-RPC responses and give darktable stderr
  fflush(stdout);
  const int saved_fd = dup(STDOUT_FILENO);
  FILE *proto_out = fdopen(saved_fd, "w");
  dup2(STDERR_FILENO, STDOUT_FILENO);

  if(dt_init(m_argc, m_arg, FALSE, TRUE, NULL))
  {
    fprintf(stderr, "darktable-mcp: dt_init failed\n");
    g_ptr_array_free(m, TRUE);
    return 1;
  }

  // tool descriptions and schemas live in the data folder, editable without a rebuild
  char datadir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));
  // refuse anything that would change the library; the other valve, an
  // in-memory catalog, also costs access to the user's images
  dt_bridge_set_read_only(read_only);

  gchar *tools_file = g_build_filename(datadir, "mcp_tools.json", NULL);
  if(mcp_tools_load(tools_file) < 0)
    fprintf(stderr, "darktable-mcp: no tools available (could not load '%s')\n",
            tools_file);
  g_free(tools_file);

  mcp_jsonrpc_loop(stdin, proto_out ? proto_out : stdout);

  dt_cleanup();
  if(proto_out) fflush(proto_out);
  g_ptr_array_free(m, TRUE);
  return 0;
}
