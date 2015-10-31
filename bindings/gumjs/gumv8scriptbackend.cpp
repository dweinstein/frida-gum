/*
 * Copyright (C) 2010-2015 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "gumv8scriptbackend.h"

#include "gumv8platform.h"

#include <string.h>
#include <v8-debug.h>

#define GUM_V8_SCRIPT_BACKEND_LOCK()   (g_mutex_lock (&priv->mutex))
#define GUM_V8_SCRIPT_BACKEND_UNLOCK() (g_mutex_unlock (&priv->mutex))

#define GUM_V8_FLAGS \
    "--es-staging " \
    "--harmony-array-includes " \
    "--harmony-regexps " \
    "--harmony-proxies " \
    "--harmony-rest-parameters " \
    "--harmony-reflect " \
    "--harmony-destructuring " \
    "--expose-gc"

using namespace v8;

typedef struct _GumV8EmitDebugMessageData GumV8EmitDebugMessageData;

template <typename T>
struct GumPersistent
{
  typedef Persistent<T, CopyablePersistentTraits<T> > type;
};

struct _GumV8ScriptBackendPrivate
{
  GMutex mutex;

  GumV8Platform * platform;

  GumScriptDebugMessageHandler debug_handler;
  gpointer debug_handler_data;
  GDestroyNotify debug_handler_data_destroy;
  GMainContext * debug_handler_context;
  GumPersistent<Context>::type * debug_context;
};

struct _GumV8EmitDebugMessageData
{
  GumV8ScriptBackend * backend;
  gchar * message;
};

static void gum_v8_script_backend_iface_init (gpointer g_iface,
    gpointer iface_data);
static void gum_v8_script_backend_dispose (GObject * object);
static void gum_v8_script_backend_finalize (GObject * object);

static void gum_v8_script_backend_create (GumScriptBackend * backend,
    const gchar * name, const gchar * source, GumScriptFlavor flavor,
    GCancellable * cancellable, GAsyncReadyCallback callback,
    gpointer user_data);
static GumScript * gum_v8_script_backend_create_finish (
    GumScriptBackend * backend, GAsyncResult * result, GError ** error);
static GumScript * gum_v8_script_backend_create_sync (
    GumScriptBackend * backend, const gchar * name, const gchar * source,
    GumScriptFlavor flavor, GCancellable * cancellable, GError ** error);

static void gum_v8_script_backend_set_debug_message_handler (
    GumScriptBackend * backend, GumScriptDebugMessageHandler handler,
    gpointer data, GDestroyNotify data_destroy);
static void gum_v8_script_backend_enable_debugger (GumV8ScriptBackend * self);
static void gum_v8_script_backend_disable_debugger (GumV8ScriptBackend * self);
static void gum_v8_script_backend_emit_debug_message (
    const Debug::Message & message);
static gboolean gum_v8_script_backend_do_emit_debug_message (
    GumV8EmitDebugMessageData * d);
static void gum_v8_emit_debug_message_data_free (GumV8EmitDebugMessageData * d);
static void gum_v8_script_backend_post_debug_message (
    GumScriptBackend * backend, const gchar * message);
static void gum_v8_script_backend_do_process_debug_messages (
    GumV8ScriptBackend * self);

static void gum_v8_script_backend_ignore (GumScriptBackend * backend,
    GumThreadId thread_id);
static void gum_v8_script_backend_unignore (GumScriptBackend * backend,
    GumThreadId thread_id);
static void gum_v8_script_backend_unignore_later (GumScriptBackend * backend,
    GumThreadId thread_id);
static gboolean gum_v8_script_backend_is_ignoring (GumScriptBackend * backend,
    GumThreadId thread_id);

G_DEFINE_TYPE_EXTENDED (GumV8ScriptBackend,
                        gum_v8_script_backend,
                        G_TYPE_OBJECT,
                        0,
                        G_IMPLEMENT_INTERFACE (GUM_TYPE_SCRIPT_BACKEND,
                            gum_v8_script_backend_iface_init));

static void
gum_v8_script_backend_class_init (GumV8ScriptBackendClass * klass)
{
  GObjectClass * object_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (GumV8ScriptBackendPrivate));

  object_class->dispose = gum_v8_script_backend_dispose;
  object_class->finalize = gum_v8_script_backend_finalize;
}

static void
gum_v8_script_backend_iface_init (gpointer g_iface,
                                  gpointer iface_data)
{
  GumScriptBackendIface * iface = (GumScriptBackendIface *) g_iface;

  (void) iface_data;

  iface->create = gum_v8_script_backend_create;
  iface->create_finish = gum_v8_script_backend_create_finish;
  iface->create_sync = gum_v8_script_backend_create_sync;

  iface->set_debug_message_handler =
      gum_v8_script_backend_set_debug_message_handler;
  iface->post_debug_message = gum_v8_script_backend_post_debug_message;

  iface->ignore = gum_v8_script_backend_ignore;
  iface->unignore = gum_v8_script_backend_unignore;
  iface->unignore_later = gum_v8_script_backend_unignore_later;
  iface->is_ignoring = gum_v8_script_backend_is_ignoring;
}

static void
gum_v8_script_backend_init (GumV8ScriptBackend * self)
{
  GumV8ScriptBackendPrivate * priv;

  priv = self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      GUM_V8_TYPE_SCRIPT_BACKEND, GumV8ScriptBackendPrivate);

  g_mutex_init (&priv->mutex);

  V8::SetFlagsFromString (GUM_V8_FLAGS,
      static_cast<int> (strlen (GUM_V8_FLAGS)));

  priv->platform = new GumV8Platform ();

  Isolate * isolate = static_cast<Isolate *> (
      gum_v8_script_backend_get_isolate (self));
  isolate->SetData (0, self);
}

static void
gum_v8_script_backend_dispose (GObject * object)
{
  GumV8ScriptBackend * self = GUM_V8_SCRIPT_BACKEND (object);
  GumV8ScriptBackendPrivate * priv = self->priv;

  if (priv->debug_handler_data_destroy != NULL)
    priv->debug_handler_data_destroy (priv->debug_handler_data);
  priv->debug_handler = NULL;
  priv->debug_handler_data = NULL;
  priv->debug_handler_data_destroy = NULL;

  g_clear_pointer (&priv->debug_handler_context, g_main_context_unref);

  gum_v8_script_backend_disable_debugger (self);

  G_OBJECT_CLASS (gum_v8_script_backend_parent_class)->dispose (object);
}

static void
gum_v8_script_backend_finalize (GObject * object)
{
  GumV8ScriptBackend * self = GUM_V8_SCRIPT_BACKEND (object);
  GumV8ScriptBackendPrivate * priv = self->priv;

  delete priv->platform;

  g_mutex_clear (&priv->mutex);

  G_OBJECT_CLASS (gum_v8_script_backend_parent_class)->finalize (object);
}

gpointer
gum_v8_script_backend_get_isolate (GumV8ScriptBackend * self)
{
  return self->priv->platform->GetIsolate ();
}

GumScriptScheduler *
gum_v8_script_backend_get_scheduler (GumV8ScriptBackend * self)
{
  return self->priv->platform->GetScheduler ();
}

static void
gum_v8_script_backend_create (GumScriptBackend * backend,
                              const gchar * name,
                              const gchar * source,
                              GumScriptFlavor flavor,
                              GCancellable * cancellable,
                              GAsyncReadyCallback callback,
                              gpointer user_data)
{
}

static GumScript *
gum_v8_script_backend_create_finish (GumScriptBackend * backend,
                                     GAsyncResult * result,
                                     GError ** error)
{
  return NULL;
}

static GumScript *
gum_v8_script_backend_create_sync (GumScriptBackend * backend,
                                   const gchar * name,
                                   const gchar * source,
                                   GumScriptFlavor flavor,
                                   GCancellable * cancellable,
                                   GError ** error)
{
  return NULL;
}

static void
gum_v8_script_backend_set_debug_message_handler (
    GumScriptBackend * backend,
    GumScriptDebugMessageHandler handler,
    gpointer data,
    GDestroyNotify data_destroy)
{
  GumV8ScriptBackend * self = GUM_V8_SCRIPT_BACKEND (backend);
  GumV8ScriptBackendPrivate * priv = self->priv;
  GMainContext * old_context, * new_context;

  if (priv->debug_handler_data_destroy != NULL)
    priv->debug_handler_data_destroy (priv->debug_handler_data);

  priv->debug_handler = handler;
  priv->debug_handler_data = data;
  priv->debug_handler_data_destroy = data_destroy;

  new_context = (handler != NULL) ? g_main_context_ref_thread_default () : NULL;

  GUM_V8_SCRIPT_BACKEND_LOCK ();
  old_context = priv->debug_handler_context;
  priv->debug_handler_context = new_context;
  GUM_V8_SCRIPT_BACKEND_UNLOCK ();

  if (old_context != NULL)
    g_main_context_unref (old_context);

  gum_script_scheduler_push_job_on_js_thread (
      gum_v8_script_backend_get_scheduler (self),
      G_PRIORITY_DEFAULT,
      (handler != NULL)
          ? (GumScriptJobFunc) gum_v8_script_backend_enable_debugger
          : (GumScriptJobFunc) gum_v8_script_backend_disable_debugger,
      self, NULL, self);
}

static void
gum_v8_script_backend_enable_debugger (GumV8ScriptBackend * self)
{
  GumV8ScriptBackendPrivate * priv = self->priv;
  Isolate * isolate = static_cast<Isolate *> (
      gum_v8_script_backend_get_isolate (self));

  Locker locker (isolate);
  Isolate::Scope isolate_scope (isolate);
  HandleScope handle_scope (isolate);

  Debug::SetMessageHandler (gum_v8_script_backend_emit_debug_message);

  Local<Context> context = Debug::GetDebugContext ();
  priv->debug_context = new GumPersistent<Context>::type (isolate, context);
  Context::Scope context_scope (context);

  gum_v8_bundle_run (priv->platform->GetDebugRuntime ());
}

static void
gum_v8_script_backend_disable_debugger (GumV8ScriptBackend * self)
{
  GumV8ScriptBackendPrivate * priv = self->priv;
  Isolate * isolate = static_cast<Isolate *> (
      gum_v8_script_backend_get_isolate (self));

  Locker locker (isolate);
  Isolate::Scope isolate_scope (isolate);
  HandleScope handle_scope (isolate);

  delete priv->debug_context;
  priv->debug_context = nullptr;

  Debug::SetMessageHandler (nullptr);
}

static void
gum_v8_script_backend_emit_debug_message (const Debug::Message & message)
{
  Isolate * isolate = message.GetIsolate ();
  GumV8ScriptBackend * self = GUM_V8_SCRIPT_BACKEND (isolate->GetData (0));
  GumV8ScriptBackendPrivate * priv = self->priv;
  HandleScope scope (isolate);

  Local<String> json = message.GetJSON ();
  String::Utf8Value json_str (json);

  GUM_V8_SCRIPT_BACKEND_LOCK ();
  GMainContext * context = (priv->debug_handler_context != NULL)
      ? g_main_context_ref (priv->debug_handler_context)
      : NULL;
  GUM_V8_SCRIPT_BACKEND_UNLOCK ();

  if (context == NULL)
    return;

  GumV8EmitDebugMessageData * d = g_slice_new (GumV8EmitDebugMessageData);
  d->backend = self;
  g_object_ref (self);
  d->message = g_strdup (*json_str);

  GSource * source = g_idle_source_new ();
  g_source_set_callback (source,
      (GSourceFunc) gum_v8_script_backend_do_emit_debug_message,
      d,
      (GDestroyNotify) gum_v8_emit_debug_message_data_free);
  g_source_attach (source, context);
  g_source_unref (source);

  g_main_context_unref (context);
}

static gboolean
gum_v8_script_backend_do_emit_debug_message (GumV8EmitDebugMessageData * d)
{
  GumV8ScriptBackend * self = d->backend;
  GumV8ScriptBackendPrivate * priv = self->priv;

  if (priv->debug_handler != NULL)
    priv->debug_handler (d->message, priv->debug_handler_data);

  return FALSE;
}

static void
gum_v8_emit_debug_message_data_free (GumV8EmitDebugMessageData * d)
{
  g_free (d->message);
  g_object_unref (d->backend);

  g_slice_free (GumV8EmitDebugMessageData, d);
}

static void
gum_v8_script_backend_post_debug_message (GumScriptBackend * backend,
                                          const gchar * message)
{
  GumV8ScriptBackend * self = GUM_V8_SCRIPT_BACKEND (backend);
  GumV8ScriptBackendPrivate * priv = self->priv;

  if (priv->debug_handler == NULL)
    return;

  Isolate * isolate = static_cast<Isolate *> (
      gum_v8_script_backend_get_isolate (self));

  glong command_length;
  uint16_t * command = g_utf8_to_utf16 (message, (glong) strlen (message), NULL,
      &command_length, NULL);
  g_assert (command != NULL);

  Debug::SendCommand (isolate, command, command_length);

  g_free (command);

  gum_script_scheduler_push_job_on_js_thread (
      gum_v8_script_backend_get_scheduler (self),
      G_PRIORITY_DEFAULT,
      (GumScriptJobFunc) gum_v8_script_backend_do_process_debug_messages,
      self, NULL, self);
}

static void
gum_v8_script_backend_do_process_debug_messages (GumV8ScriptBackend * self)
{
  GumV8ScriptBackendPrivate * priv = self->priv;
  Isolate * isolate = static_cast<Isolate *> (
      gum_v8_script_backend_get_isolate (self));
  Locker locker (isolate);
  Isolate::Scope isolate_scope (isolate);
  HandleScope handle_scope (isolate);
  Local<Context> context (Local<Context>::New (isolate, *priv->debug_context));
  Context::Scope context_scope (context);

  Debug::ProcessDebugMessages ();
}

static void
gum_v8_script_backend_ignore (GumScriptBackend * backend,
                              GumThreadId thread_id)
{
}

static void
gum_v8_script_backend_unignore (GumScriptBackend * backend,
                                GumThreadId thread_id)
{
}

static void
gum_v8_script_backend_unignore_later (GumScriptBackend * backend,
                                      GumThreadId thread_id)
{
}

static gboolean
gum_v8_script_backend_is_ignoring (GumScriptBackend * backend,
                                   GumThreadId thread_id)
{
  return FALSE;
}