/* gbp-lldb-debugger.c
 *
 * Copyright 2026 GNOME Builder contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * This debugger drives LLDB through `lldb-dap`, which speaks the Debug
 * Adapter Protocol: JSON messages framed with a Content-Length header over
 * the stdin/stdout of the adapter process.
 */

#define G_LOG_DOMAIN "gbp-lldb-debugger"

#include "config.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <json-glib/json-glib.h>

#include <libide-core.h>
#include <libide-foundry.h>
#include <libide-io.h>
#include <libide-threading.h>

#include "gbp-lldb-debugger.h"

#define READ_BUFFER_LEN 4096

/* Time to wait for the debug manager to hand us its breakpoints after the
 * adapter told us it is ready, before we start the program.
 */
#define CONFIGURATION_DELAY_MSEC 200

#define FRAME_ID_KEY "gbp-lldb-frame-id"

struct _GbpLldbDebugger
{
  IdeDebugger             parent_instance;

  GIOStream              *io_stream;
  GOutputStream          *inferior_out;
  GQueue                  inferior_queue;  /* GBytes waiting to be written */
  GCancellable           *read_cancellable;
  GByteArray             *rx;
  guint8                  read_buffer[READ_BUFFER_LEN];

  GHashTable             *pending;      /* gint64 *seq -> IdeTask */
  GHashTable             *breakpoints;  /* path -> GPtrArray<IdeDebuggerBreakpoint> */
  GHashTable             *threads;      /* id string -> IdeDebuggerThread */
  IdeDebuggerThreadGroup *group;

  char                   *program;
  char                  **args;
  char                   *cwd;
  char                  **env;

  GPid                    inferior_pid;
  gint64                  seq;
  gint64                  current_thread;
  gint64                  top_frame_id;
  guint                   next_breakpoint_id;
  guint                   configure_source;

  guint                   has_connected : 1;
  guint                   configured : 1;
  guint                   exited : 1;
  guint                   inferior_busy : 1;
  guint                   program_exited : 1;
};

G_DEFINE_FINAL_TYPE (GbpLldbDebugger, gbp_lldb_debugger, IDE_TYPE_DEBUGGER)

typedef struct
{
  GbpLldbDebugger      *self;
  IdeDebuggerStopReason reason;
  gint64                thread_id;
} StopData;

static void gbp_lldb_debugger_process_events (GbpLldbDebugger *self,
                                              JsonObject      *message);

/* ------------------------------------------------------------------ */
/* JSON helpers                                                        */
/* ------------------------------------------------------------------ */

static JsonObject *
get_object_member (JsonObject *object,
                   const char *name)
{
  if (object != NULL && json_object_has_member (object, name) &&
      JSON_NODE_HOLDS_OBJECT (json_object_get_member (object, name)))
    return json_object_get_object_member (object, name);
  return NULL;
}

static JsonArray *
get_array_member (JsonObject *object,
                  const char *name)
{
  if (object != NULL && json_object_has_member (object, name) &&
      JSON_NODE_HOLDS_ARRAY (json_object_get_member (object, name)))
    return json_object_get_array_member (object, name);
  return NULL;
}

static const char *
get_string_member (JsonObject *object,
                   const char *name)
{
  if (object != NULL && json_object_has_member (object, name) &&
      JSON_NODE_HOLDS_VALUE (json_object_get_member (object, name)))
    return json_object_get_string_member (object, name);
  return NULL;
}

static gint64
get_int_member (JsonObject *object,
                const char *name,
                gint64      default_value)
{
  if (object != NULL && json_object_has_member (object, name) &&
      JSON_NODE_HOLDS_VALUE (json_object_get_member (object, name)))
    return json_object_get_int_member (object, name);
  return default_value;
}

static JsonNode *
thread_arguments (gint64 thread_id)
{
  g_autoptr(JsonBuilder) builder = json_builder_new ();

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "threadId");
  json_builder_add_int_value (builder, thread_id);
  json_builder_end_object (builder);

  return json_builder_get_root (builder);
}

/* When the run is stopped, lldb-dap is killed and takes neither debugserver
 * nor the program with it. Do not leave the program behind, stopped and with
 * nothing to continue it.
 */
static void
gbp_lldb_debugger_kill_orphan (GbpLldbDebugger *self)
{
  if (self->inferior_pid > 0 && !self->program_exited)
    {
      kill (self->inferior_pid, SIGKILL);
      self->program_exited = TRUE;
    }
}

/* ------------------------------------------------------------------ */
/* Transport                                                           */
/* ------------------------------------------------------------------ */

static gboolean
gbp_lldb_debugger_check_ready (GbpLldbDebugger  *self,
                               GError          **error)
{
  if (self->io_stream == NULL)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_INVAL,
                   "The connection to lldb-dap has not been set");
      return FALSE;
    }

  if (g_io_stream_is_closed (self->io_stream))
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_CLOSED,
                   "The connection is closed");
      return FALSE;
    }

  return TRUE;
}

static gboolean
gbp_lldb_debugger_write_message (GbpLldbDebugger  *self,
                                 JsonNode         *node,
                                 GError          **error)
{
  g_autoptr(JsonGenerator) generator = NULL;
  g_autofree char *body = NULL;
  g_autofree char *header = NULL;
  GOutputStream *out;
  gsize len = 0;

  generator = json_generator_new ();
  json_generator_set_root (generator, node);
  body = json_generator_to_data (generator, &len);
  header = g_strdup_printf ("Content-Length: %" G_GSIZE_FORMAT "\r\n\r\n", len);

  out = g_io_stream_get_output_stream (self->io_stream);

  return g_output_stream_write_all (out, header, strlen (header), NULL, NULL, error) &&
         g_output_stream_write_all (out, body, len, NULL, NULL, error) &&
         g_output_stream_flush (out, NULL, error);
}

static void
gbp_lldb_debugger_request_async (GbpLldbDebugger    *self,
                                 const char         *command,
                                 JsonNode           *arguments,
                                 GCancellable       *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer            user_data)
{
  g_autoptr(IdeTask) task = NULL;
  g_autoptr(JsonNode) args = arguments;
  g_autoptr(JsonBuilder) builder = NULL;
  g_autoptr(JsonNode) message = NULL;
  g_autoptr(GError) error = NULL;
  gint64 *key;
  gint64 seq;

  g_assert (GBP_IS_LLDB_DEBUGGER (self));
  g_assert (command != NULL);

  task = ide_task_new (self, cancellable, callback, user_data);
  ide_task_set_source_tag (task, gbp_lldb_debugger_request_async);

  if (!gbp_lldb_debugger_check_ready (self, &error))
    {
      ide_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  seq = ++self->seq;

  builder = json_builder_new ();
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "seq");
  json_builder_add_int_value (builder, seq);
  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (builder, "request");
  json_builder_set_member_name (builder, "command");
  json_builder_add_string_value (builder, command);
  if (args != NULL)
    {
      json_builder_set_member_name (builder, "arguments");
      json_builder_add_value (builder, json_node_ref (args));
    }
  json_builder_end_object (builder);
  message = json_builder_get_root (builder);

  g_debug ("-> %s (seq %" G_GINT64_FORMAT ")", command, seq);

  if (!gbp_lldb_debugger_write_message (self, message, &error))
    {
      ide_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  key = g_new (gint64, 1);
  *key = seq;
  g_hash_table_insert (self->pending, key, g_steal_pointer (&task));
}

/* Returns the full response message on success */
static JsonNode *
gbp_lldb_debugger_request_finish (GbpLldbDebugger  *self,
                                  GAsyncResult     *result,
                                  GError          **error)
{
  g_assert (GBP_IS_LLDB_DEBUGGER (self));
  g_assert (IDE_IS_TASK (result));

  return ide_task_propagate_pointer (IDE_TASK (result), error);
}

static void
ignore_response_cb (GObject      *object,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  GbpLldbDebugger *self = (GbpLldbDebugger *)object;
  g_autoptr(JsonNode) response = NULL;
  g_autoptr(GError) error = NULL;

  if (!(response = gbp_lldb_debugger_request_finish (self, result, &error)) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_debug ("Request failed: %s", error->message);
}

static void
gbp_lldb_debugger_send (GbpLldbDebugger *self,
                        const char      *command,
                        JsonNode        *arguments)
{
  gbp_lldb_debugger_request_async (self, command, arguments, NULL, ignore_response_cb, NULL);
}

static void
gbp_lldb_debugger_dispatch_response (GbpLldbDebugger *self,
                                     JsonNode        *node)
{
  JsonObject *message = json_node_get_object (node);
  g_autoptr(IdeTask) task = NULL;
  gint64 request_seq = get_int_member (message, "request_seq", -1);
  gboolean success = FALSE;

  if (json_object_has_member (message, "success"))
    success = json_object_get_boolean_member (message, "success");

  if (!g_hash_table_steal_extended (self->pending, &request_seq, NULL, (gpointer *)&task))
    return;

  if (success)
    {
      ide_task_return_pointer (task, json_node_copy (node), json_node_unref);
    }
  else
    {
      const char *text = get_string_member (message, "message");

      ide_task_return_new_error (task,
                                 G_IO_ERROR,
                                 G_IO_ERROR_FAILED,
                                 "%s: %s",
                                 get_string_member (message, "command") ?: "request",
                                 text ?: "failed");
    }
}

static void
gbp_lldb_debugger_process_buffer (GbpLldbDebugger *self)
{
  for (;;)
    {
      g_autoptr(JsonParser) parser = NULL;
      g_autoptr(GError) error = NULL;
      g_autofree char *header = NULL;
      g_autofree char *lower = NULL;
      const char *length_str;
      const char *separator;
      gsize header_len;
      gsize body_len;
      gsize total;

      separator = memmem (self->rx->data, self->rx->len, "\r\n\r\n", 4);
      if (separator == NULL)
        return;

      header_len = separator - (const char *)self->rx->data;
      header = g_strndup ((const char *)self->rx->data, header_len);
      lower = g_ascii_strdown (header, -1);

      if (!(length_str = strstr (lower, "content-length:")))
        {
          g_warning ("Received DAP message without Content-Length");
          g_byte_array_set_size (self->rx, 0);
          return;
        }

      body_len = g_ascii_strtoull (length_str + strlen ("content-length:"), NULL, 10);
      total = header_len + 4 + body_len;

      if (self->rx->len < total)
        return;

      parser = json_parser_new ();

      if (json_parser_load_from_data (parser,
                                      (const char *)self->rx->data + header_len + 4,
                                      body_len,
                                      &error))
        {
          JsonNode *root = json_parser_get_root (parser);

          if (root != NULL && JSON_NODE_HOLDS_OBJECT (root))
            {
              JsonObject *message = json_node_get_object (root);
              const char *type = get_string_member (message, "type");

              if (g_strcmp0 (type, "response") == 0)
                gbp_lldb_debugger_dispatch_response (self, root);
              else if (g_strcmp0 (type, "event") == 0)
                gbp_lldb_debugger_process_events (self, message);
            }
        }
      else
        {
          g_warning ("Failed to parse DAP message: %s", error->message);
        }

      g_byte_array_remove_range (self->rx, 0, total);
    }
}

/* ------------------------------------------------------------------ */
/* Threads, breakpoints and stop handling                              */
/* ------------------------------------------------------------------ */

static void
gbp_lldb_debugger_ensure_group (GbpLldbDebugger *self)
{
  if (self->group != NULL)
    return;

  self->group = ide_debugger_thread_group_new ("i1");
  ide_debugger_emit_thread_group_added (IDE_DEBUGGER (self), self->group);
  ide_debugger_emit_thread_group_started (IDE_DEBUGGER (self), self->group);
}

static IdeDebuggerThread *
gbp_lldb_debugger_ensure_thread (GbpLldbDebugger *self,
                                 gint64           thread_id)
{
  g_autofree char *id = g_strdup_printf ("%" G_GINT64_FORMAT, thread_id);
  IdeDebuggerThread *thread;

  if ((thread = g_hash_table_lookup (self->threads, id)))
    return thread;

  gbp_lldb_debugger_ensure_group (self);

  thread = ide_debugger_thread_new (id);
  ide_debugger_thread_set_group (thread, ide_debugger_thread_group_get_id (self->group));
  g_hash_table_insert (self->threads, g_steal_pointer (&id), thread);
  ide_debugger_emit_thread_added (IDE_DEBUGGER (self), thread);

  return thread;
}

static void
gbp_lldb_debugger_threads_cb (GObject      *object,
                              GAsyncResult *result,
                              gpointer      user_data)
{
  GbpLldbDebugger *self = (GbpLldbDebugger *)object;
  g_autoptr(JsonNode) response = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GHashTable) alive = NULL;
  GHashTableIter iter;
  gpointer key;
  JsonArray *threads;

  g_assert (GBP_IS_LLDB_DEBUGGER (self));

  if (!(response = gbp_lldb_debugger_request_finish (self, result, &error)))
    return;

  alive = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  threads = get_array_member (get_object_member (json_node_get_object (response), "body"), "threads");

  if (threads != NULL)
    {
      for (guint i = 0; i < json_array_get_length (threads); i++)
        {
          JsonObject *t = json_array_get_object_element (threads, i);
          gint64 id = get_int_member (t, "id", 0);

          gbp_lldb_debugger_ensure_thread (self, id);
          g_hash_table_add (alive, g_strdup_printf ("%" G_GINT64_FORMAT, id));
        }
    }

  g_hash_table_iter_init (&iter, self->threads);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      if (!g_hash_table_contains (alive, key))
        {
          g_autoptr(IdeDebuggerThread) thread = g_object_ref (g_hash_table_lookup (self->threads, key));

          g_hash_table_iter_remove (&iter);
          ide_debugger_emit_thread_removed (IDE_DEBUGGER (self), thread);
        }
    }
}

static IdeDebuggerBreakpoint *
gbp_lldb_debugger_copy_breakpoint (GbpLldbDebugger      *self,
                                   IdeDebuggerBreakpoint *breakpoint)
{
  g_autofree char *id = g_strdup_printf ("%u", ++self->next_breakpoint_id);
  IdeDebuggerBreakpoint *copy = ide_debugger_breakpoint_new (id);

  ide_debugger_breakpoint_set_file (copy, ide_debugger_breakpoint_get_file (breakpoint));
  ide_debugger_breakpoint_set_line (copy, ide_debugger_breakpoint_get_line (breakpoint));
  ide_debugger_breakpoint_set_enabled (copy, ide_debugger_breakpoint_get_enabled (breakpoint));
  ide_debugger_breakpoint_set_mode (copy, IDE_DEBUGGER_BREAK_BREAKPOINT);

  return copy;
}

static void
gbp_lldb_debugger_sync_file (GbpLldbDebugger *self,
                             const char      *path,
                             GPtrArray       *breakpoints)
{
  g_autoptr(JsonBuilder) builder = json_builder_new ();

  g_assert (GBP_IS_LLDB_DEBUGGER (self));
  g_assert (path != NULL);

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "source");
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "path");
  json_builder_add_string_value (builder, path);
  json_builder_end_object (builder);
  json_builder_set_member_name (builder, "breakpoints");
  json_builder_begin_array (builder);

  if (breakpoints != NULL)
    {
      for (guint i = 0; i < breakpoints->len; i++)
        {
          IdeDebuggerBreakpoint *bp = g_ptr_array_index (breakpoints, i);

          if (!ide_debugger_breakpoint_get_enabled (bp))
            continue;

          json_builder_begin_object (builder);
          json_builder_set_member_name (builder, "line");
          json_builder_add_int_value (builder, ide_debugger_breakpoint_get_line (bp));
          json_builder_end_object (builder);
        }
    }

  json_builder_end_array (builder);
  json_builder_end_object (builder);

  gbp_lldb_debugger_send (self, "setBreakpoints", json_builder_get_root (builder));
}

static gboolean
gbp_lldb_debugger_configure_cb (gpointer data)
{
  GbpLldbDebugger *self = data;
  GHashTableIter iter;
  gpointer key;
  gpointer value;

  g_assert (GBP_IS_LLDB_DEBUGGER (self));

  self->configure_source = 0;

  g_hash_table_iter_init (&iter, self->breakpoints);
  while (g_hash_table_iter_next (&iter, &key, &value))
    gbp_lldb_debugger_sync_file (self, key, value);

  self->configured = TRUE;
  gbp_lldb_debugger_send (self, "configurationDone", NULL);

  return G_SOURCE_REMOVE;
}

static IdeDebuggerStopReason
parse_stop_reason (const char *reason)
{
  if (g_strcmp0 (reason, "breakpoint") == 0 ||
      g_strcmp0 (reason, "function breakpoint") == 0 ||
      g_strcmp0 (reason, "data breakpoint") == 0 ||
      g_strcmp0 (reason, "instruction breakpoint") == 0)
    return IDE_DEBUGGER_STOP_BREAKPOINT_HIT;

  if (g_strcmp0 (reason, "step") == 0 ||
      g_strcmp0 (reason, "pause") == 0 ||
      g_strcmp0 (reason, "entry") == 0 ||
      g_strcmp0 (reason, "goto") == 0)
    return IDE_DEBUGGER_STOP_LOCATION_REACHED;

  if (g_strcmp0 (reason, "exception") == 0 ||
      g_strcmp0 (reason, "signal") == 0)
    return IDE_DEBUGGER_STOP_SIGNAL_RECEIVED;

  return IDE_DEBUGGER_STOP_UNKNOWN;
}

static void
stop_data_free (StopData *data)
{
  g_clear_object (&data->self);
  g_free (data);
}

static void
gbp_lldb_debugger_stop_frame_cb (GObject      *object,
                                 GAsyncResult *result,
                                 gpointer      user_data)
{
  GbpLldbDebugger *self = (GbpLldbDebugger *)object;
  StopData *data = user_data;
  g_autoptr(IdeDebuggerBreakpoint) location = NULL;
  g_autoptr(JsonNode) response = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *thread_id = NULL;
  IdeDebuggerThread *thread;
  JsonArray *frames;

  g_assert (GBP_IS_LLDB_DEBUGGER (self));
  g_assert (data != NULL);

  thread_id = g_strdup_printf ("%" G_GINT64_FORMAT, data->thread_id);
  location = ide_debugger_breakpoint_new ("stop");
  ide_debugger_breakpoint_set_thread (location, thread_id);

  if ((response = gbp_lldb_debugger_request_finish (self, result, &error)) &&
      (frames = get_array_member (get_object_member (json_node_get_object (response), "body"), "stackFrames")) &&
      json_array_get_length (frames) > 0)
    {
      JsonObject *frame = json_array_get_object_element (frames, 0);
      JsonObject *source = get_object_member (frame, "source");
      const char *path = get_string_member (source, "path");
      const char *ip = get_string_member (frame, "instructionPointerReference");

      self->top_frame_id = get_int_member (frame, "id", 0);

      if (path != NULL)
        ide_debugger_breakpoint_set_file (location, path);
      ide_debugger_breakpoint_set_line (location, get_int_member (frame, "line", 0));
      ide_debugger_breakpoint_set_function (location, get_string_member (frame, "name"));
      if (ip != NULL)
        ide_debugger_breakpoint_set_address (location, ide_debugger_address_parse (ip));
    }

  thread = gbp_lldb_debugger_ensure_thread (self, data->thread_id);
  ide_debugger_emit_thread_selected (IDE_DEBUGGER (self), thread);
  ide_debugger_emit_stopped (IDE_DEBUGGER (self), data->reason, location);

  stop_data_free (data);
}

static void
gbp_lldb_debugger_handle_stopped (GbpLldbDebugger *self,
                                  JsonObject      *body)
{
  g_autoptr(JsonBuilder) builder = json_builder_new ();
  StopData *data;

  g_assert (GBP_IS_LLDB_DEBUGGER (self));

  self->current_thread = get_int_member (body, "threadId", self->current_thread);
  self->top_frame_id = 0;

  data = g_new0 (StopData, 1);
  data->self = g_object_ref (self);
  data->reason = parse_stop_reason (get_string_member (body, "reason"));
  data->thread_id = self->current_thread;

  gbp_lldb_debugger_request_async (self, "threads", NULL, NULL, gbp_lldb_debugger_threads_cb, NULL);

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "threadId");
  json_builder_add_int_value (builder, self->current_thread);
  json_builder_set_member_name (builder, "startFrame");
  json_builder_add_int_value (builder, 0);
  json_builder_set_member_name (builder, "levels");
  json_builder_add_int_value (builder, 1);
  json_builder_end_object (builder);

  gbp_lldb_debugger_request_async (self,
                                   "stackTrace",
                                   json_builder_get_root (builder),
                                   NULL,
                                   gbp_lldb_debugger_stop_frame_cb,
                                   data);
}

static void
gbp_lldb_debugger_emit_exited (GbpLldbDebugger      *self,
                               IdeDebuggerStopReason reason)
{
  g_autoptr(IdeDebuggerBreakpoint) location = NULL;

  if (self->exited)
    return;

  self->exited = TRUE;

  if (self->group != NULL)
    ide_debugger_emit_thread_group_exited (IDE_DEBUGGER (self), self->group);

  location = ide_debugger_breakpoint_new ("exit");
  ide_debugger_emit_stopped (IDE_DEBUGGER (self), reason, location);
}

/* ------------------------------------------------------------------ */
/* Program output                                                      */
/* ------------------------------------------------------------------ */

static void gbp_lldb_debugger_write_next_output (GbpLldbDebugger *self);

static void
gbp_lldb_debugger_log_target (GbpLldbDebugger *self,
                              GBytes          *bytes)
{
  ide_debugger_emit_log (IDE_DEBUGGER (self), IDE_DEBUGGER_TARGET, bytes);
}

static void
gbp_lldb_debugger_write_output_cb (GObject      *object,
                                   GAsyncResult *result,
                                   gpointer      user_data)
{
  GOutputStream *stream = (GOutputStream *)object;
  g_autoptr(GbpLldbDebugger) self = user_data;
  g_autoptr(GError) error = NULL;

  g_assert (G_IS_OUTPUT_STREAM (stream));
  g_assert (GBP_IS_LLDB_DEBUGGER (self));

  self->inferior_busy = FALSE;

  if (!g_output_stream_write_all_finish (stream, result, NULL, &error))
    {
      /* The terminal went away. Show the rest in the debugger log. */
      g_debug ("Failed to write program output: %s", error->message);
      g_clear_object (&self->inferior_out);
    }

  gbp_lldb_debugger_write_next_output (self);
}

static void
gbp_lldb_debugger_write_next_output (GbpLldbDebugger *self)
{
  GBytes *bytes;

  g_assert (GBP_IS_LLDB_DEBUGGER (self));

  if (self->inferior_busy)
    return;

  while ((bytes = g_queue_pop_head (&self->inferior_queue)))
    {
      if (self->inferior_out == NULL)
        {
          gbp_lldb_debugger_log_target (self, bytes);
          continue;
        }

      /* Writing must not block the main loop: the terminal that drains this
       * PTY runs on it, so a blocking write on a full PTY would deadlock.
       */
      self->inferior_busy = TRUE;
      g_output_stream_write_all_async (self->inferior_out,
                                       g_bytes_get_data (bytes, NULL),
                                       g_bytes_get_size (bytes),
                                       G_PRIORITY_DEFAULT,
                                       NULL,
                                       gbp_lldb_debugger_write_output_cb,
                                       g_object_ref (self));
      /* The data must stay valid until the write completes */
      g_object_set_data_full (G_OBJECT (self->inferior_out),
                              "gbp-lldb-pending",
                              bytes,
                              (GDestroyNotify)g_bytes_unref);
      return;
    }
}

static void
gbp_lldb_debugger_write_output (GbpLldbDebugger *self,
                                const char      *data,
                                gsize            len)
{
  g_assert (GBP_IS_LLDB_DEBUGGER (self));

  if (self->inferior_out == NULL)
    {
      gbp_lldb_debugger_log_target (self, g_bytes_new (data, len));
      return;
    }

  g_queue_push_tail (&self->inferior_queue, g_bytes_new (data, len));
  gbp_lldb_debugger_write_next_output (self);
}

static void
gbp_lldb_debugger_process_events (GbpLldbDebugger *self,
                                  JsonObject      *message)
{
  const char *event = get_string_member (message, "event");
  JsonObject *body = get_object_member (message, "body");

  g_assert (GBP_IS_LLDB_DEBUGGER (self));

  if (event == NULL)
    return;

  g_debug ("<- event %s", event);

  if (g_strcmp0 (event, "initialized") == 0)
    {
      /* The adapter is ready for breakpoints. Give the debug manager a
       * moment to register the ones it has cached, then start the program.
       */
      if (self->configure_source == 0)
        self->configure_source = g_timeout_add (CONFIGURATION_DELAY_MSEC,
                                                gbp_lldb_debugger_configure_cb,
                                                self);
    }
  else if (g_strcmp0 (event, "process") == 0)
    {
      gint64 pid = get_int_member (body, "systemProcessId", 0);

      gbp_lldb_debugger_ensure_group (self);

      if (pid > 0)
        {
          g_autofree char *str = g_strdup_printf ("%" G_GINT64_FORMAT, pid);

          self->inferior_pid = pid;
          ide_debugger_thread_group_set_pid (self->group, str);
        }

      /* The base class only allows movement once the program has started */
      ide_debugger_emit_running (IDE_DEBUGGER (self));
    }
  else if (g_strcmp0 (event, "stopped") == 0)
    {
      gbp_lldb_debugger_handle_stopped (self, body);
    }
  else if (g_strcmp0 (event, "continued") == 0)
    {
      ide_debugger_emit_running (IDE_DEBUGGER (self));
    }
  else if (g_strcmp0 (event, "thread") == 0)
    {
      gint64 id = get_int_member (body, "threadId", 0);

      if (g_strcmp0 (get_string_member (body, "reason"), "started") == 0)
        gbp_lldb_debugger_ensure_thread (self, id);
    }
  else if (g_strcmp0 (event, "output") == 0)
    {
      const char *category = get_string_member (body, "category");
      const char *output = get_string_member (body, "output");

      if (output == NULL)
        return;

      if (category == NULL || g_strcmp0 (category, "console") == 0 ||
          g_strcmp0 (category, "important") == 0 || g_strcmp0 (category, "telemetry") == 0)
        {
          if (g_strcmp0 (category, "telemetry") != 0)
            ide_debugger_emit_log (IDE_DEBUGGER (self),
                                   IDE_DEBUGGER_CONSOLE,
                                   g_bytes_new (output, strlen (output)));
        }
      else
        {
          gbp_lldb_debugger_write_output (self, output, strlen (output));
        }
    }
  else if (g_strcmp0 (event, "exited") == 0)
    {
      gint64 code = get_int_member (body, "exitCode", 0);
      g_autofree char *str = g_strdup_printf ("%" G_GINT64_FORMAT, code);

      self->program_exited = TRUE;

      gbp_lldb_debugger_ensure_group (self);
      ide_debugger_thread_group_set_exit_code (self->group, str);
      gbp_lldb_debugger_emit_exited (self,
                                     code == 0 ? IDE_DEBUGGER_STOP_EXITED_NORMALLY
                                               : IDE_DEBUGGER_STOP_EXITED);
    }
  else if (g_strcmp0 (event, "terminated") == 0)
    {
      self->program_exited = TRUE;
      gbp_lldb_debugger_emit_exited (self, IDE_DEBUGGER_STOP_EXITED_NORMALLY);

      /* Ask the adapter to go away so the run command can complete */
      if (!g_io_stream_is_closed (self->io_stream))
        {
          g_autoptr(JsonBuilder) builder = json_builder_new ();

          json_builder_begin_object (builder);
          json_builder_set_member_name (builder, "terminateDebuggee");
          json_builder_add_boolean_value (builder, FALSE);
          json_builder_end_object (builder);

          gbp_lldb_debugger_send (self, "disconnect", json_builder_get_root (builder));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Reading                                                             */
/* ------------------------------------------------------------------ */

static void
gbp_lldb_debugger_read_cb (GObject      *object,
                           GAsyncResult *result,
                           gpointer      user_data)
{
  GInputStream *stream = (GInputStream *)object;
  g_autoptr(GbpLldbDebugger) self = user_data;
  g_autoptr(GError) error = NULL;
  gssize n_read;

  g_assert (G_IS_INPUT_STREAM (stream));
  g_assert (GBP_IS_LLDB_DEBUGGER (self));

  n_read = g_input_stream_read_finish (stream, result, &error);

  if (n_read <= 0)
    {
      if (error != NULL && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("Failed to read from lldb-dap: %s", error->message);

      /* The adapter is gone, so no more responses will arrive */
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          GHashTableIter iter;
          gpointer task;

          g_hash_table_iter_init (&iter, self->pending);
          while (g_hash_table_iter_next (&iter, NULL, &task))
            {
              ide_task_return_new_error (g_object_ref (task),
                                         G_IO_ERROR,
                                         G_IO_ERROR_CLOSED,
                                         "The connection to lldb-dap was closed");
              g_hash_table_iter_remove (&iter);
            }

          gbp_lldb_debugger_kill_orphan (self);
          gbp_lldb_debugger_emit_exited (self, IDE_DEBUGGER_STOP_EXITED);
        }

      return;
    }

  g_byte_array_append (self->rx, self->read_buffer, n_read);
  gbp_lldb_debugger_process_buffer (self);

  g_input_stream_read_async (stream,
                             self->read_buffer,
                             READ_BUFFER_LEN,
                             G_PRIORITY_LOW,
                             self->read_cancellable,
                             gbp_lldb_debugger_read_cb,
                             g_object_ref (self));
}

static void
gbp_lldb_debugger_connect (GbpLldbDebugger *self,
                           GIOStream       *io_stream)
{
  GInputStream *stream;

  g_assert (GBP_IS_LLDB_DEBUGGER (self));
  g_assert (G_IS_IO_STREAM (io_stream));
  g_assert (self->io_stream == NULL);

  self->has_connected = TRUE;
  g_set_object (&self->io_stream, io_stream);

  stream = g_io_stream_get_input_stream (self->io_stream);

  g_input_stream_read_async (stream,
                             self->read_buffer,
                             READ_BUFFER_LEN,
                             G_PRIORITY_LOW,
                             self->read_cancellable,
                             gbp_lldb_debugger_read_cb,
                             g_object_ref (self));
}

/* ------------------------------------------------------------------ */
/* Starting the program                                                */
/* ------------------------------------------------------------------ */

static void
gbp_lldb_debugger_launch_cb (GObject      *object,
                             GAsyncResult *result,
                             gpointer      user_data)
{
  GbpLldbDebugger *self = (GbpLldbDebugger *)object;
  g_autoptr(JsonNode) response = NULL;
  g_autoptr(GError) error = NULL;

  g_assert (GBP_IS_LLDB_DEBUGGER (self));

  if (!(response = gbp_lldb_debugger_request_finish (self, result, &error)) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      g_autofree char *msg = g_strdup_printf ("Failed to launch: %s\n", error->message);

      g_warning ("%s", msg);
      ide_debugger_emit_log (IDE_DEBUGGER (self),
                             IDE_DEBUGGER_CONSOLE,
                             g_bytes_new (msg, strlen (msg)));
      gbp_lldb_debugger_emit_exited (self, IDE_DEBUGGER_STOP_EXITED);
    }
}

static void
gbp_lldb_debugger_start (GbpLldbDebugger *self)
{
  g_autoptr(JsonBuilder) builder = json_builder_new ();

  g_assert (GBP_IS_LLDB_DEBUGGER (self));

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "clientID");
  json_builder_add_string_value (builder, "gnome-builder");
  json_builder_set_member_name (builder, "clientName");
  json_builder_add_string_value (builder, "GNOME Builder");
  json_builder_set_member_name (builder, "adapterID");
  json_builder_add_string_value (builder, "lldb-dap");
  json_builder_set_member_name (builder, "linesStartAt1");
  json_builder_add_boolean_value (builder, TRUE);
  json_builder_set_member_name (builder, "columnsStartAt1");
  json_builder_add_boolean_value (builder, TRUE);
  json_builder_set_member_name (builder, "pathFormat");
  json_builder_add_string_value (builder, "path");
  json_builder_set_member_name (builder, "supportsRunInTerminalRequest");
  json_builder_add_boolean_value (builder, FALSE);
  json_builder_end_object (builder);

  gbp_lldb_debugger_send (self, "initialize", json_builder_get_root (builder));

  g_clear_object (&builder);
  builder = json_builder_new ();

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "program");
  json_builder_add_string_value (builder, self->program ? self->program : "");
  json_builder_set_member_name (builder, "args");
  json_builder_begin_array (builder);
  for (guint i = 0; self->args != NULL && self->args[i] != NULL; i++)
    json_builder_add_string_value (builder, self->args[i]);
  json_builder_end_array (builder);
  if (self->cwd != NULL)
    {
      json_builder_set_member_name (builder, "cwd");
      json_builder_add_string_value (builder, self->cwd);
    }
  json_builder_set_member_name (builder, "env");
  json_builder_begin_array (builder);
  for (guint i = 0; self->env != NULL && self->env[i] != NULL; i++)
    json_builder_add_string_value (builder, self->env[i]);
  json_builder_end_array (builder);
  json_builder_set_member_name (builder, "stopOnEntry");
  json_builder_add_boolean_value (builder, FALSE);
  json_builder_end_object (builder);

  gbp_lldb_debugger_request_async (self,
                                   "launch",
                                   json_builder_get_root (builder),
                                   NULL,
                                   gbp_lldb_debugger_launch_cb,
                                   NULL);
}

static gboolean
gbp_lldb_debugger_run_context_handler_cb (IdeRunContext       *run_context,
                                          const char * const  *argv,
                                          const char * const  *env,
                                          const char          *cwd,
                                          IdeUnixFDMap        *unix_fd_map,
                                          gpointer             user_data,
                                          GError             **error)
{
  GbpLldbDebugger *self = user_data;
  g_autoptr(GIOStream) io_stream = NULL;
  int pty_fd;

  IDE_ENTRY;

  g_assert (GBP_IS_LLDB_DEBUGGER (self));
  g_assert (IDE_IS_RUN_CONTEXT (run_context));
  g_assert (argv != NULL);
  g_assert (env != NULL);
  g_assert (IDE_IS_UNIX_FD_MAP (unix_fd_map));

  if (argv[0] == NULL)
    {
      g_set_error_literal (error,
                           G_IO_ERROR,
                           G_IO_ERROR_INVAL,
                           "No program to debug");
      IDE_RETURN (FALSE);
    }

  /* Remember what to launch. The program is started through the adapter
   * instead of being spawned directly.
   */
  g_free (self->program);
  g_strfreev (self->args);
  g_free (self->cwd);
  g_strfreev (self->env);
  self->program = g_strdup (argv[0]);
  self->args = g_strdupv ((char **)&argv[1]);
  self->cwd = g_strdup (cwd);
  self->env = g_strdupv ((char **)env);

  /* The PTY of the run pane is for the program's output, which the adapter
   * reports through output events. Keep it so we can write that output to
   * the terminal.
   */
  g_clear_object (&self->inferior_out);
  if ((pty_fd = ide_unix_fd_map_steal_stdout (unix_fd_map)) != -1)
    self->inferior_out = g_unix_output_stream_new (pty_fd, TRUE);

  /* The adapter speaks over regular pipes on stdin/stdout, and its stderr
   * must not be merged into the stream.
   */
  if (!(io_stream = ide_unix_fd_map_create_stream (unix_fd_map, STDIN_FILENO, STDOUT_FILENO, error)) ||
      !ide_unix_fd_map_silence_fd (unix_fd_map, STDERR_FILENO, error))
    IDE_RETURN (FALSE);

  if (!ide_run_context_merge_unix_fd_map (run_context, unix_fd_map, error))
    IDE_RETURN (FALSE);

  /* lldb-dap is not in PATH, it is only reachable through xcrun */
  ide_run_context_append_argv (run_context, "xcrun");
  ide_run_context_append_argv (run_context, "lldb-dap");

  gbp_lldb_debugger_connect (self, io_stream);
  ide_debugger_move_async (IDE_DEBUGGER (self),
                           IDE_DEBUGGER_MOVEMENT_START,
                           NULL, NULL, NULL);

  IDE_RETURN (TRUE);
}

/* ------------------------------------------------------------------ */
/* IdeDebugger virtual functions                                       */
/* ------------------------------------------------------------------ */

static gboolean
gbp_lldb_debugger_supports_run_command (IdeDebugger   *debugger,
                                        IdePipeline   *pipeline,
                                        IdeRunCommand *run_command,
                                        int           *priority)
{
  g_assert (GBP_IS_LLDB_DEBUGGER (debugger));
  g_assert (IDE_IS_PIPELINE (pipeline));
  g_assert (IDE_IS_RUN_COMMAND (run_command));
  g_assert (priority != NULL);

  *priority = 100;

  return TRUE;
}

static void
gbp_lldb_debugger_prepare_for_run (IdeDebugger   *debugger,
                                   IdePipeline   *pipeline,
                                   IdeRunContext *run_context)
{
  GbpLldbDebugger *self = (GbpLldbDebugger *)debugger;

  g_assert (GBP_IS_LLDB_DEBUGGER (self));
  g_assert (IDE_IS_PIPELINE (pipeline));
  g_assert (IDE_IS_RUN_CONTEXT (run_context));

  ide_run_context_push (run_context,
                        gbp_lldb_debugger_run_context_handler_cb,
                        g_object_ref (self),
                        g_object_unref);
}

static void
gbp_lldb_debugger_move_cb (GObject      *object,
                           GAsyncResult *result,
                           gpointer      user_data)
{
  GbpLldbDebugger *self = (GbpLldbDebugger *)object;
  g_autoptr(IdeTask) task = user_data;
  g_autoptr(JsonNode) response = NULL;
  g_autoptr(GError) error = NULL;

  g_assert (GBP_IS_LLDB_DEBUGGER (self));
  g_assert (IDE_IS_TASK (task));

  if (!(response = gbp_lldb_debugger_request_finish (self, result, &error)))
    ide_task_return_error (task, g_steal_pointer (&error));
  else
    ide_task_return_boolean (task, TRUE);
}

static void
gbp_lldb_debugger_move_async (IdeDebugger         *debugger,
                              IdeDebuggerMovement  movement,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
  GbpLldbDebugger *self = (GbpLldbDebugger *)debugger;
  g_autoptr(IdeTask) task = NULL;
  const char *command = NULL;

  g_assert (GBP_IS_LLDB_DEBUGGER (self));
  g_assert (IDE_IS_DEBUGGER_MOVEMENT (movement));

  task = ide_task_new (self, cancellable, callback, user_data);
  ide_task_set_source_tag (task, gbp_lldb_debugger_move_async);

  switch (movement)
    {
    case IDE_DEBUGGER_MOVEMENT_START:
      gbp_lldb_debugger_start (self);
      ide_task_return_boolean (task, TRUE);
      return;

    case IDE_DEBUGGER_MOVEMENT_CONTINUE:
      command = "continue";
      break;

    case IDE_DEBUGGER_MOVEMENT_STEP_IN:
      command = "stepIn";
      break;

    case IDE_DEBUGGER_MOVEMENT_STEP_OVER:
      command = "next";
      break;

    case IDE_DEBUGGER_MOVEMENT_FINISH:
      command = "stepOut";
      break;

    default:
      ide_task_return_new_error (task,
                                 G_IO_ERROR,
                                 G_IO_ERROR_NOT_SUPPORTED,
                                 "Unsupported movement");
      return;
    }

  gbp_lldb_debugger_request_async (self,
                                   command,
                                   thread_arguments (self->current_thread),
                                   cancellable,
                                   gbp_lldb_debugger_move_cb,
                                   g_steal_pointer (&task));
}

static gboolean
gbp_lldb_debugger_move_finish (IdeDebugger   *debugger,
                               GAsyncResult  *result,
                               GError       **error)
{
  g_assert (GBP_IS_LLDB_DEBUGGER (debugger));
  g_assert (IDE_IS_TASK (result));

  return ide_task_propagate_boolean (IDE_TASK (result), error);
}

static void
gbp_lldb_debugger_list_breakpoints_async (IdeDebugger         *debugger,
                                          GCancellable        *cancellable,
                                          GAsyncReadyCallback  callback,
                                          gpointer             user_data)
{
  GbpLldbDebugger *self = (GbpLldbDebugger *)debugger;
  g_autoptr(IdeTask) task = NULL;
  GPtrArray *ret;
  GHashTableIter iter;
  gpointer value;

  g_assert (GBP_IS_LLDB_DEBUGGER (self));

  task = ide_task_new (self, cancellable, callback, user_data);
  ide_task_set_source_tag (task, gbp_lldb_debugger_list_breakpoints_async);

  ret = g_ptr_array_new_with_free_func (g_object_unref);

  g_hash_table_iter_init (&iter, self->breakpoints);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      GPtrArray *array = value;

      for (guint i = 0; i < array->len; i++)
        g_ptr_array_add (ret, g_object_ref (g_ptr_array_index (array, i)));
    }

  ide_task_return_pointer (task, ret, g_ptr_array_unref);
}

static GPtrArray *
gbp_lldb_debugger_list_breakpoints_finish (IdeDebugger   *debugger,
                                           GAsyncResult  *result,
                                           GError       **error)
{
  GPtrArray *ret;

  g_assert (GBP_IS_LLDB_DEBUGGER (debugger));
  g_assert (IDE_IS_TASK (result));

  ret = ide_task_propagate_pointer (IDE_TASK (result), error);

  return IDE_PTR_ARRAY_STEAL_FULL (&ret);
}

static void
gbp_lldb_debugger_insert_breakpoint_async (IdeDebugger           *debugger,
                                           IdeDebuggerBreakpoint *breakpoint,
                                           GCancellable          *cancellable,
                                           GAsyncReadyCallback    callback,
                                           gpointer               user_data)
{
  GbpLldbDebugger *self = (GbpLldbDebugger *)debugger;
  g_autoptr(IdeDebuggerBreakpoint) copy = NULL;
  g_autoptr(IdeTask) task = NULL;
  GPtrArray *array;
  const char *file;
  guint line;

  g_assert (GBP_IS_LLDB_DEBUGGER (self));
  g_assert (IDE_IS_DEBUGGER_BREAKPOINT (breakpoint));

  task = ide_task_new (self, cancellable, callback, user_data);
  ide_task_set_source_tag (task, gbp_lldb_debugger_insert_breakpoint_async);

  file = ide_debugger_breakpoint_get_file (breakpoint);
  line = ide_debugger_breakpoint_get_line (breakpoint);

  if (file == NULL || line == 0)
    {
      ide_task_return_new_error (task,
                                 G_IO_ERROR,
                                 G_IO_ERROR_NOT_SUPPORTED,
                                 "Only breakpoints on a file and line are supported");
      return;
    }

  if (!(array = g_hash_table_lookup (self->breakpoints, file)))
    {
      array = g_ptr_array_new_with_free_func (g_object_unref);
      g_hash_table_insert (self->breakpoints, g_strdup (file), array);
    }

  /* Replace any breakpoint already on this line */
  for (guint i = 0; i < array->len; i++)
    {
      IdeDebuggerBreakpoint *existing = g_ptr_array_index (array, i);

      if (ide_debugger_breakpoint_get_line (existing) == line)
        {
          g_ptr_array_remove_index (array, i);
          break;
        }
    }

  copy = gbp_lldb_debugger_copy_breakpoint (self, breakpoint);
  g_ptr_array_add (array, g_object_ref (copy));

  if (self->configured)
    gbp_lldb_debugger_sync_file (self, file, array);

  ide_debugger_emit_breakpoint_added (IDE_DEBUGGER (self), copy);

  ide_task_return_boolean (task, TRUE);
}

static gboolean
gbp_lldb_debugger_insert_breakpoint_finish (IdeDebugger   *debugger,
                                            GAsyncResult  *result,
                                            GError       **error)
{
  g_assert (GBP_IS_LLDB_DEBUGGER (debugger));
  g_assert (IDE_IS_TASK (result));

  return ide_task_propagate_boolean (IDE_TASK (result), error);
}

static void
gbp_lldb_debugger_remove_breakpoint_async (IdeDebugger           *debugger,
                                           IdeDebuggerBreakpoint *breakpoint,
                                           GCancellable          *cancellable,
                                           GAsyncReadyCallback    callback,
                                           gpointer               user_data)
{
  GbpLldbDebugger *self = (GbpLldbDebugger *)debugger;
  g_autoptr(IdeDebuggerBreakpoint) removed = NULL;
  g_autoptr(IdeTask) task = NULL;
  GPtrArray *array;
  const char *file;
  guint line;

  g_assert (GBP_IS_LLDB_DEBUGGER (self));
  g_assert (IDE_IS_DEBUGGER_BREAKPOINT (breakpoint));

  task = ide_task_new (self, cancellable, callback, user_data);
  ide_task_set_source_tag (task, gbp_lldb_debugger_remove_breakpoint_async);

  file = ide_debugger_breakpoint_get_file (breakpoint);
  line = ide_debugger_breakpoint_get_line (breakpoint);

  if (file != NULL && (array = g_hash_table_lookup (self->breakpoints, file)))
    {
      for (guint i = 0; i < array->len; i++)
        {
          IdeDebuggerBreakpoint *existing = g_ptr_array_index (array, i);

          if (existing == breakpoint || ide_debugger_breakpoint_get_line (existing) == line)
            {
              removed = g_object_ref (existing);
              g_ptr_array_remove_index (array, i);
              break;
            }
        }

      if (self->configured)
        gbp_lldb_debugger_sync_file (self, file, array);
    }

  ide_debugger_emit_breakpoint_removed (IDE_DEBUGGER (self), removed ? removed : breakpoint);

  ide_task_return_boolean (task, TRUE);
}

static gboolean
gbp_lldb_debugger_remove_breakpoint_finish (IdeDebugger   *debugger,
                                            GAsyncResult  *result,
                                            GError       **error)
{
  g_assert (GBP_IS_LLDB_DEBUGGER (debugger));
  g_assert (IDE_IS_TASK (result));

  return ide_task_propagate_boolean (IDE_TASK (result), error);
}

static void
gbp_lldb_debugger_list_frames_cb (GObject      *object,
                                  GAsyncResult *result,
                                  gpointer      user_data)
{
  GbpLldbDebugger *self = (GbpLldbDebugger *)object;
  g_autoptr(IdeTask) task = user_data;
  g_autoptr(JsonNode) response = NULL;
  g_autoptr(GError) error = NULL;
  GPtrArray *ret;
  JsonArray *frames;

  g_assert (GBP_IS_LLDB_DEBUGGER (self));
  g_assert (IDE_IS_TASK (task));

  if (!(response = gbp_lldb_debugger_request_finish (self, result, &error)))
    {
      ide_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  ret = g_ptr_array_new_with_free_func (g_object_unref);
  frames = get_array_member (get_object_member (json_node_get_object (response), "body"), "stackFrames");

  for (guint i = 0; frames != NULL && i < json_array_get_length (frames); i++)
    {
      JsonObject *f = json_array_get_object_element (frames, i);
      JsonObject *source = get_object_member (f, "source");
      const char *ip = get_string_member (f, "instructionPointerReference");
      IdeDebuggerFrame *frame = ide_debugger_frame_new ();
      gint64 *id = g_new (gint64, 1);

      *id = get_int_member (f, "id", 0);
      g_object_set_data_full (G_OBJECT (frame), FRAME_ID_KEY, id, g_free);

      ide_debugger_frame_set_depth (frame, i);
      ide_debugger_frame_set_function (frame, get_string_member (f, "name"));
      ide_debugger_frame_set_file (frame, get_string_member (source, "path"));
      ide_debugger_frame_set_line (frame, get_int_member (f, "line", 0));
      if (ip != NULL)
        ide_debugger_frame_set_address (frame, ide_debugger_address_parse (ip));

      g_ptr_array_add (ret, frame);
    }

  ide_task_return_pointer (task, ret, g_ptr_array_unref);
}

static void
gbp_lldb_debugger_list_frames_async (IdeDebugger         *debugger,
                                     IdeDebuggerThread   *thread,
                                     GCancellable        *cancellable,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data)
{
  GbpLldbDebugger *self = (GbpLldbDebugger *)debugger;
  g_autoptr(IdeTask) task = NULL;
  g_autoptr(JsonBuilder) builder = json_builder_new ();
  gint64 thread_id = self->current_thread;

  g_assert (GBP_IS_LLDB_DEBUGGER (self));
  g_assert (!thread || IDE_IS_DEBUGGER_THREAD (thread));

  task = ide_task_new (self, cancellable, callback, user_data);
  ide_task_set_source_tag (task, gbp_lldb_debugger_list_frames_async);

  if (thread != NULL)
    thread_id = g_ascii_strtoll (ide_debugger_thread_get_id (thread), NULL, 10);

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "threadId");
  json_builder_add_int_value (builder, thread_id);
  json_builder_set_member_name (builder, "startFrame");
  json_builder_add_int_value (builder, 0);
  json_builder_set_member_name (builder, "levels");
  json_builder_add_int_value (builder, 64);
  json_builder_end_object (builder);

  gbp_lldb_debugger_request_async (self,
                                   "stackTrace",
                                   json_builder_get_root (builder),
                                   cancellable,
                                   gbp_lldb_debugger_list_frames_cb,
                                   g_steal_pointer (&task));
}

static GPtrArray *
gbp_lldb_debugger_list_frames_finish (IdeDebugger   *debugger,
                                      GAsyncResult  *result,
                                      GError       **error)
{
  GPtrArray *ret;

  g_assert (GBP_IS_LLDB_DEBUGGER (debugger));
  g_assert (IDE_IS_TASK (result));

  ret = ide_task_propagate_pointer (IDE_TASK (result), error);

  return IDE_PTR_ARRAY_STEAL_FULL (&ret);
}

static void
gbp_lldb_debugger_variables_cb (GObject      *object,
                                GAsyncResult *result,
                                gpointer      user_data)
{
  GbpLldbDebugger *self = (GbpLldbDebugger *)object;
  g_autoptr(IdeTask) task = user_data;
  g_autoptr(JsonNode) response = NULL;
  g_autoptr(GError) error = NULL;
  GPtrArray *ret;
  JsonArray *variables;

  g_assert (GBP_IS_LLDB_DEBUGGER (self));
  g_assert (IDE_IS_TASK (task));

  if (!(response = gbp_lldb_debugger_request_finish (self, result, &error)))
    {
      ide_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  ret = g_ptr_array_new_with_free_func (g_object_unref);
  variables = get_array_member (get_object_member (json_node_get_object (response), "body"), "variables");

  for (guint i = 0; variables != NULL && i < json_array_get_length (variables); i++)
    {
      JsonObject *v = json_array_get_object_element (variables, i);
      IdeDebuggerVariable *variable = ide_debugger_variable_new (get_string_member (v, "name"));

      ide_debugger_variable_set_type_name (variable, get_string_member (v, "type"));
      ide_debugger_variable_set_value (variable, get_string_member (v, "value"));
      ide_debugger_variable_set_has_children (variable, get_int_member (v, "variablesReference", 0) > 0);

      g_ptr_array_add (ret, variable);
    }

  ide_task_return_pointer (task, ret, g_ptr_array_unref);
}

static void
gbp_lldb_debugger_scopes_cb (GObject      *object,
                             GAsyncResult *result,
                             gpointer      user_data)
{
  GbpLldbDebugger *self = (GbpLldbDebugger *)object;
  g_autoptr(IdeTask) task = user_data;
  g_autoptr(JsonNode) response = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(JsonBuilder) builder = NULL;
  JsonArray *scopes;
  gint64 reference = 0;

  g_assert (GBP_IS_LLDB_DEBUGGER (self));
  g_assert (IDE_IS_TASK (task));

  if (!(response = gbp_lldb_debugger_request_finish (self, result, &error)))
    {
      ide_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  scopes = get_array_member (get_object_member (json_node_get_object (response), "body"), "scopes");

  for (guint i = 0; scopes != NULL && i < json_array_get_length (scopes); i++)
    {
      JsonObject *scope = json_array_get_object_element (scopes, i);

      if (g_strcmp0 (get_string_member (scope, "presentationHint"), "locals") == 0 ||
          g_strcmp0 (get_string_member (scope, "name"), "Locals") == 0)
        {
          reference = get_int_member (scope, "variablesReference", 0);
          break;
        }
    }

  if (reference == 0)
    {
      ide_task_return_pointer (task,
                               g_ptr_array_new_with_free_func (g_object_unref),
                               g_ptr_array_unref);
      return;
    }

  builder = json_builder_new ();
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "variablesReference");
  json_builder_add_int_value (builder, reference);
  json_builder_end_object (builder);

  gbp_lldb_debugger_request_async (self,
                                   "variables",
                                   json_builder_get_root (builder),
                                   NULL,
                                   gbp_lldb_debugger_variables_cb,
                                   g_steal_pointer (&task));
}

static void
gbp_lldb_debugger_list_locals_async (IdeDebugger         *debugger,
                                     IdeDebuggerThread   *thread,
                                     IdeDebuggerFrame    *frame,
                                     GCancellable        *cancellable,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data)
{
  GbpLldbDebugger *self = (GbpLldbDebugger *)debugger;
  g_autoptr(IdeTask) task = NULL;
  g_autoptr(JsonBuilder) builder = json_builder_new ();
  gint64 *frame_id = NULL;

  g_assert (GBP_IS_LLDB_DEBUGGER (self));

  task = ide_task_new (self, cancellable, callback, user_data);
  ide_task_set_source_tag (task, gbp_lldb_debugger_list_locals_async);

  if (frame != NULL)
    frame_id = g_object_get_data (G_OBJECT (frame), FRAME_ID_KEY);

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "frameId");
  json_builder_add_int_value (builder, frame_id ? *frame_id : self->top_frame_id);
  json_builder_end_object (builder);

  gbp_lldb_debugger_request_async (self,
                                   "scopes",
                                   json_builder_get_root (builder),
                                   cancellable,
                                   gbp_lldb_debugger_scopes_cb,
                                   g_steal_pointer (&task));
}

static GPtrArray *
gbp_lldb_debugger_list_locals_finish (IdeDebugger   *debugger,
                                      GAsyncResult  *result,
                                      GError       **error)
{
  GPtrArray *ret;

  g_assert (GBP_IS_LLDB_DEBUGGER (debugger));
  g_assert (IDE_IS_TASK (result));

  ret = ide_task_propagate_pointer (IDE_TASK (result), error);

  return IDE_PTR_ARRAY_STEAL_FULL (&ret);
}

/* lldb reports the arguments together with the locals */
static void
gbp_lldb_debugger_list_params_async (IdeDebugger         *debugger,
                                     IdeDebuggerThread   *thread,
                                     IdeDebuggerFrame    *frame,
                                     GCancellable        *cancellable,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data)
{
  g_autoptr(IdeTask) task = NULL;

  g_assert (GBP_IS_LLDB_DEBUGGER (debugger));

  task = ide_task_new (debugger, cancellable, callback, user_data);
  ide_task_set_source_tag (task, gbp_lldb_debugger_list_params_async);
  ide_task_return_pointer (task,
                           g_ptr_array_new_with_free_func (g_object_unref),
                           g_ptr_array_unref);
}

static GPtrArray *
gbp_lldb_debugger_list_params_finish (IdeDebugger   *debugger,
                                      GAsyncResult  *result,
                                      GError       **error)
{
  GPtrArray *ret;

  g_assert (GBP_IS_LLDB_DEBUGGER (debugger));
  g_assert (IDE_IS_TASK (result));

  ret = ide_task_propagate_pointer (IDE_TASK (result), error);

  return IDE_PTR_ARRAY_STEAL_FULL (&ret);
}

static void
gbp_lldb_debugger_interpret_cb (GObject      *object,
                                GAsyncResult *result,
                                gpointer      user_data)
{
  GbpLldbDebugger *self = (GbpLldbDebugger *)object;
  g_autoptr(IdeTask) task = user_data;
  g_autoptr(JsonNode) response = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *text = NULL;
  const char *output;

  g_assert (GBP_IS_LLDB_DEBUGGER (self));
  g_assert (IDE_IS_TASK (task));

  if (!(response = gbp_lldb_debugger_request_finish (self, result, &error)))
    {
      text = g_strdup_printf ("%s\n", error->message);
      ide_debugger_emit_log (IDE_DEBUGGER (self),
                             IDE_DEBUGGER_CONSOLE,
                             g_bytes_new (text, strlen (text)));
      ide_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  output = get_string_member (get_object_member (json_node_get_object (response), "body"), "result");

  if (output != NULL && output[0] != 0)
    {
      text = g_strdup_printf ("%s\n", output);
      ide_debugger_emit_log (IDE_DEBUGGER (self),
                             IDE_DEBUGGER_CONSOLE,
                             g_bytes_new (text, strlen (text)));
    }

  ide_task_return_boolean (task, TRUE);
}

static void
gbp_lldb_debugger_interpret_async (IdeDebugger         *debugger,
                                   const char          *command,
                                   GCancellable        *cancellable,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data)
{
  GbpLldbDebugger *self = (GbpLldbDebugger *)debugger;
  g_autoptr(IdeTask) task = NULL;
  g_autoptr(JsonBuilder) builder = json_builder_new ();

  g_assert (GBP_IS_LLDB_DEBUGGER (self));
  g_assert (command != NULL);

  task = ide_task_new (self, cancellable, callback, user_data);
  ide_task_set_source_tag (task, gbp_lldb_debugger_interpret_async);

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "expression");
  json_builder_add_string_value (builder, command);
  json_builder_set_member_name (builder, "context");
  json_builder_add_string_value (builder, "repl");
  if (self->top_frame_id != 0)
    {
      json_builder_set_member_name (builder, "frameId");
      json_builder_add_int_value (builder, self->top_frame_id);
    }
  json_builder_end_object (builder);

  gbp_lldb_debugger_request_async (self,
                                   "evaluate",
                                   json_builder_get_root (builder),
                                   cancellable,
                                   gbp_lldb_debugger_interpret_cb,
                                   g_steal_pointer (&task));
}

static gboolean
gbp_lldb_debugger_interpret_finish (IdeDebugger   *debugger,
                                    GAsyncResult  *result,
                                    GError       **error)
{
  g_assert (GBP_IS_LLDB_DEBUGGER (debugger));
  g_assert (IDE_IS_TASK (result));

  return ide_task_propagate_boolean (IDE_TASK (result), error);
}

static void
gbp_lldb_debugger_interrupt_async (IdeDebugger            *debugger,
                                   IdeDebuggerThreadGroup *thread_group,
                                   GCancellable           *cancellable,
                                   GAsyncReadyCallback     callback,
                                   gpointer                user_data)
{
  GbpLldbDebugger *self = (GbpLldbDebugger *)debugger;
  g_autoptr(IdeTask) task = NULL;

  g_assert (GBP_IS_LLDB_DEBUGGER (self));

  task = ide_task_new (self, cancellable, callback, user_data);
  ide_task_set_source_tag (task, gbp_lldb_debugger_interrupt_async);

  gbp_lldb_debugger_request_async (self,
                                   "pause",
                                   thread_arguments (self->current_thread),
                                   cancellable,
                                   gbp_lldb_debugger_move_cb,
                                   g_steal_pointer (&task));
}

static gboolean
gbp_lldb_debugger_interrupt_finish (IdeDebugger   *debugger,
                                    GAsyncResult  *result,
                                    GError       **error)
{
  g_assert (GBP_IS_LLDB_DEBUGGER (debugger));
  g_assert (IDE_IS_TASK (result));

  return ide_task_propagate_boolean (IDE_TASK (result), error);
}

/* ------------------------------------------------------------------ */
/* GObject                                                             */
/* ------------------------------------------------------------------ */

static void
gbp_lldb_debugger_destroy (IdeObject *object)
{
  GbpLldbDebugger *self = (GbpLldbDebugger *)object;

  if (self->configure_source != 0)
    {
      g_source_remove (self->configure_source);
      self->configure_source = 0;
    }

  if (self->read_cancellable != NULL)
    g_cancellable_cancel (self->read_cancellable);

  gbp_lldb_debugger_kill_orphan (self);

  g_queue_clear_full (&self->inferior_queue, (GDestroyNotify)g_bytes_unref);

  if (self->pending != NULL)
    {
      GHashTableIter iter;
      gpointer task;

      g_hash_table_iter_init (&iter, self->pending);
      while (g_hash_table_iter_next (&iter, NULL, &task))
        {
          ide_task_return_new_error (g_object_ref (task),
                                     G_IO_ERROR,
                                     G_IO_ERROR_CANCELLED,
                                     "The debugger was disposed");
          g_hash_table_iter_remove (&iter);
        }
    }

  if (self->io_stream != NULL)
    g_io_stream_close (self->io_stream, NULL, NULL);

  g_clear_object (&self->io_stream);
  g_clear_object (&self->inferior_out);
  g_clear_object (&self->group);
  g_clear_pointer (&self->breakpoints, g_hash_table_unref);
  g_clear_pointer (&self->threads, g_hash_table_unref);

  IDE_OBJECT_CLASS (gbp_lldb_debugger_parent_class)->destroy (object);
}

static void
gbp_lldb_debugger_finalize (GObject *object)
{
  GbpLldbDebugger *self = (GbpLldbDebugger *)object;

  g_clear_pointer (&self->rx, g_byte_array_unref);
  g_clear_pointer (&self->pending, g_hash_table_unref);
  g_clear_object (&self->read_cancellable);
  g_clear_pointer (&self->program, g_free);
  g_clear_pointer (&self->args, g_strfreev);
  g_clear_pointer (&self->cwd, g_free);
  g_clear_pointer (&self->env, g_strfreev);

  G_OBJECT_CLASS (gbp_lldb_debugger_parent_class)->finalize (object);
}

static void
gbp_lldb_debugger_class_init (GbpLldbDebuggerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  IdeObjectClass *i_object_class = IDE_OBJECT_CLASS (klass);
  IdeDebuggerClass *debugger_class = IDE_DEBUGGER_CLASS (klass);

  object_class->finalize = gbp_lldb_debugger_finalize;

  i_object_class->destroy = gbp_lldb_debugger_destroy;

  debugger_class->supports_run_command = gbp_lldb_debugger_supports_run_command;
  debugger_class->prepare_for_run = gbp_lldb_debugger_prepare_for_run;
  debugger_class->move_async = gbp_lldb_debugger_move_async;
  debugger_class->move_finish = gbp_lldb_debugger_move_finish;
  debugger_class->list_breakpoints_async = gbp_lldb_debugger_list_breakpoints_async;
  debugger_class->list_breakpoints_finish = gbp_lldb_debugger_list_breakpoints_finish;
  debugger_class->insert_breakpoint_async = gbp_lldb_debugger_insert_breakpoint_async;
  debugger_class->insert_breakpoint_finish = gbp_lldb_debugger_insert_breakpoint_finish;
  debugger_class->remove_breakpoint_async = gbp_lldb_debugger_remove_breakpoint_async;
  debugger_class->remove_breakpoint_finish = gbp_lldb_debugger_remove_breakpoint_finish;
  debugger_class->list_frames_async = gbp_lldb_debugger_list_frames_async;
  debugger_class->list_frames_finish = gbp_lldb_debugger_list_frames_finish;
  debugger_class->list_locals_async = gbp_lldb_debugger_list_locals_async;
  debugger_class->list_locals_finish = gbp_lldb_debugger_list_locals_finish;
  debugger_class->list_params_async = gbp_lldb_debugger_list_params_async;
  debugger_class->list_params_finish = gbp_lldb_debugger_list_params_finish;
  debugger_class->interpret_async = gbp_lldb_debugger_interpret_async;
  debugger_class->interpret_finish = gbp_lldb_debugger_interpret_finish;
  debugger_class->interrupt_async = gbp_lldb_debugger_interrupt_async;
  debugger_class->interrupt_finish = gbp_lldb_debugger_interrupt_finish;
}

static void
gbp_lldb_debugger_init (GbpLldbDebugger *self)
{
  g_queue_init (&self->inferior_queue);
  self->rx = g_byte_array_new ();
  self->read_cancellable = g_cancellable_new ();
  self->pending = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, g_object_unref);
  self->breakpoints = g_hash_table_new_full (g_str_hash,
                                             g_str_equal,
                                             g_free,
                                             (GDestroyNotify)g_ptr_array_unref);
  self->threads = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);

  ide_debugger_set_display_name (IDE_DEBUGGER (self), "LLDB");
}
