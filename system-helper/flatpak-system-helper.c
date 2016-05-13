/*
 * Copyright © 2014 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <gio/gio.h>
#include <polkit/polkit.h>

#include "flatpak-dbus.h"
#include "flatpak-dir.h"
#include "lib/flatpak-error.h"

static PolkitAuthority *authority = NULL;
static FlatpakSystemHelper *helper = NULL;
static GMainLoop *main_loop = NULL;
static guint name_owner_id = 0;

#define IDLE_TIMEOUT_SECS 10*60

/* This uses a weird Auto prefix to avoid conflicts with later added polkit types.
 */
typedef PolkitAuthorizationResult AutoPolkitAuthorizationResult;
typedef PolkitDetails AutoPolkitDetails;
typedef PolkitSubject AutoPolkitSubject;

G_DEFINE_AUTOPTR_CLEANUP_FUNC (AutoPolkitAuthorizationResult, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (AutoPolkitDetails, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (AutoPolkitSubject, g_object_unref)

static void
skeleton_died_cb (gpointer data)
{
  g_debug ("skeleton finalized, exiting");
  g_main_loop_quit (main_loop);
}

static gboolean
unref_skeleton_in_timeout_cb (gpointer user_data)
{
  static gboolean unreffed = FALSE;
  g_debug ("unreffing helper main ref");
  if (!unreffed)
    {
      g_object_unref (helper);
      unreffed = TRUE;
    }

  return G_SOURCE_REMOVE;
}

static void
unref_skeleton_in_timeout (void)
{
  if (name_owner_id)
    g_bus_unown_name (name_owner_id);
  name_owner_id = 0;

  /* After we've lost the name or idled we drop the main ref on the helper
     so that we'll exit when it drops to zero. However, if there are
     outstanding calls these will keep the refcount up during the
     execution of them. We do the unref on a timeout to make sure
     we're completely draining the queue of (stale) requests. */
  g_timeout_add (500, unref_skeleton_in_timeout_cb, NULL);
}

static gboolean
idle_timeout_cb (gpointer user_data)
{
  if (name_owner_id)
    {
      g_debug ("Idle - unowning name");
      unref_skeleton_in_timeout ();
    }
  return G_SOURCE_REMOVE;
}

G_LOCK_DEFINE_STATIC (idle);
static void
schedule_idle_callback (void)
{
  static guint idle_timeout_id = 0;

  G_LOCK(idle);

  if (idle_timeout_id != 0)
    g_source_remove (idle_timeout_id);

  idle_timeout_id = g_timeout_add_seconds (IDLE_TIMEOUT_SECS, idle_timeout_cb, NULL);

  G_UNLOCK(idle);
}


static gboolean
handle_deploy (FlatpakSystemHelper   *object,
               GDBusMethodInvocation *invocation,
               const gchar           *arg_repo_path,
               guint32                arg_flags,
               const gchar           *arg_ref,
               const gchar           *arg_origin,
               const gchar *const    *arg_subpaths)
{
  g_autoptr(FlatpakDir) system = flatpak_dir_get_system ();
  g_autoptr(GFile) path = g_file_new_for_path (arg_repo_path);
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) deploy_dir = NULL;
  gboolean is_update;
  g_autoptr(GMainContext) main_context = NULL;

  g_debug ("Deploy %s %u %s %s", arg_repo_path, arg_flags, arg_ref, arg_origin);

  if ((arg_flags & ~FLATPAK_HELPER_DEPLOY_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", (arg_flags & ~FLATPAK_HELPER_DEPLOY_FLAGS_ALL));
      return TRUE;
    }

  if (!g_file_query_exists (path, NULL))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "Path does not exist");
      return TRUE;
    }

  is_update = (arg_flags & FLATPAK_HELPER_DEPLOY_FLAGS_UPDATE) != 0;

  deploy_dir = flatpak_dir_get_if_deployed (system, arg_ref,
                                            NULL, NULL);

  if (deploy_dir)
    {
      g_autofree char *real_origin = NULL;
      if (!is_update)
        {
          /* Can't install already installed app */
          g_dbus_method_invocation_return_error (invocation, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED,
                                                 "%s is already installed", arg_ref);
          return TRUE;
        }

      real_origin = flatpak_dir_get_origin (system, arg_ref, NULL, NULL);
      if (g_strcmp0 (real_origin, arg_origin) != 0)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                 "Wrong origin %s for update", arg_origin);
          return TRUE;
        }
    }
  else if (!deploy_dir && is_update)
    {
      /* Can't update not installed app */
      g_dbus_method_invocation_return_error (invocation, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED,
                                             "%s is not installed", arg_ref);
      return TRUE;
    }

  if (!flatpak_dir_ensure_repo (system, NULL, &error))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                             "Can't open system repo %s", error->message);
      return TRUE;
    }

  /* Work around ostree-pull spinning the default main context for the sync calls */
  main_context = g_main_context_new ();
  g_main_context_push_thread_default (main_context);

  if (!flatpak_dir_pull_untrusted_local (system, arg_repo_path,
                                         arg_origin,
                                         arg_ref,
                                         (char **) arg_subpaths,
                                         NULL,
                                         NULL, &error))
    {
      g_main_context_pop_thread_default (main_context);
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                             "Error pulling from repo: %s", error->message);
      return TRUE;
    }

  g_main_context_pop_thread_default (main_context);

  if (is_update)
    {
      /* TODO: This doesn't support a custom subpath */
      if (!flatpak_dir_deploy_update (system, arg_ref,
                                      NULL,
                                      NULL, &error))
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Error deploying: %s", error->message);
          return TRUE;
        }
    }
  else
    {
      if (!flatpak_dir_deploy_install (system, arg_ref, arg_origin,
                                       (char **) arg_subpaths,
                                       NULL, &error))
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Error deploying: %s", error->message);
          return TRUE;
        }
    }

  flatpak_system_helper_complete_deploy (object, invocation);

  return TRUE;
}

static gboolean
handle_deploy_appstream (FlatpakSystemHelper   *object,
                         GDBusMethodInvocation *invocation,
                         const gchar           *arg_repo_path,
                         const gchar           *arg_origin,
                         const gchar           *arg_arch)
{
  g_autoptr(FlatpakDir) system = flatpak_dir_get_system ();
  g_autoptr(GFile) path = g_file_new_for_path (arg_repo_path);
  g_autoptr(GError) error = NULL;
  g_autoptr(GMainContext) main_context = NULL;
  g_autofree char *branch = NULL;

  g_debug ("DeployAppstream %s %s %s", arg_repo_path, arg_origin, arg_arch);

  if (!g_file_query_exists (path, NULL))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "Path does not exist");
      return TRUE;
    }

  if (!flatpak_dir_ensure_repo (system, NULL, &error))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                             "Can't open system repo %s", error->message);
      return TRUE;
    }

  /* Work around ostree-pull spinning the default main context for the sync calls */
  main_context = g_main_context_new ();
  g_main_context_push_thread_default (main_context);

  branch = g_strdup_printf ("appstream/%s", arg_arch);

  if (!flatpak_dir_pull_untrusted_local (system, arg_repo_path,
                                         arg_origin,
                                         branch,
                                         NULL,
                                         NULL,
                                         NULL, &error))
    {
      g_main_context_pop_thread_default (main_context);
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                             "Error pulling from repo: %s", error->message);
      return TRUE;
    }

  g_main_context_pop_thread_default (main_context);

  if (!flatpak_dir_deploy_appstream (system,
                                     arg_origin,
                                     arg_arch,
                                     NULL,
                                     NULL,
                                     &error))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                             "Error deploying appstream: %s", error->message);
      return TRUE;
    }

  flatpak_system_helper_complete_deploy_appstream (object, invocation);

  return TRUE;
}

static gboolean
handle_uninstall (FlatpakSystemHelper *object,
                  GDBusMethodInvocation *invocation,
                  guint arg_flags,
                  const gchar *arg_ref)
{
  g_autoptr(FlatpakDir) system = flatpak_dir_get_system ();
  g_autoptr(GError) error = NULL;
  g_autoptr(GMainContext) main_context = NULL;

  g_debug ("Uninstall %u %s", arg_flags, arg_ref);

  if ((arg_flags & ~FLATPAK_HELPER_UNINSTALL_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", (arg_flags & ~FLATPAK_HELPER_UNINSTALL_FLAGS_ALL));
      return TRUE;
    }

  if (!flatpak_dir_ensure_repo (system, NULL, &error))
    {
      g_dbus_method_invocation_return_gerror  (invocation, error);
      return TRUE;
    }

  if (!flatpak_dir_uninstall (system, arg_ref, arg_flags, NULL, &error))
    {
      g_dbus_method_invocation_return_gerror  (invocation, error);
      return TRUE;
    }

  flatpak_system_helper_complete_uninstall (object, invocation);
  return TRUE;
}

static gboolean
flatpak_authorize_method_handler (GDBusInterfaceSkeleton *interface,
                                  GDBusMethodInvocation  *invocation,
                                  gpointer                user_data)
{
  const gchar *method_name;
  GVariant *parameters;
  gboolean authorized;
  const gchar *sender;
  const gchar *action;

  /* Ensure we don't idle exit */
  schedule_idle_callback ();

  g_autoptr(AutoPolkitSubject) subject = NULL;
  g_autoptr(AutoPolkitDetails) details = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(AutoPolkitAuthorizationResult) result = NULL;

  authorized = FALSE;

  method_name = g_dbus_method_invocation_get_method_name (invocation);
  parameters = g_dbus_method_invocation_get_parameters (invocation);

  sender = g_dbus_method_invocation_get_sender (invocation);
  subject = polkit_system_bus_name_new (sender);

  if (g_strcmp0 (method_name, "Deploy") == 0)
    {
      const char *ref, *origin;
      guint32 flags;
      gboolean is_update;
      gboolean is_app;

      g_variant_get_child (parameters, 1, "u", &flags);
      g_variant_get_child (parameters, 2, "&s", &ref);
      g_variant_get_child (parameters, 3, "&s", &origin);

      is_update = (flags & FLATPAK_HELPER_DEPLOY_FLAGS_UPDATE) != 0;
      is_app = g_str_has_prefix (ref, "app/");

      if (is_update)
        {
          if (is_app)
            action = "org.freedesktop.Flatpak.app-update";
          else
            action = "org.freedesktop.Flatpak.runtime-update";
        }
      else
        {
          if (is_app)
            action = "org.freedesktop.Flatpak.app-install";
          else
            action = "org.freedesktop.Flatpak.runtime-install";
        }

      details = polkit_details_new ();
      polkit_details_insert (details, "origin", origin);
      polkit_details_insert (details, "ref", ref);

      result = polkit_authority_check_authorization_sync (authority, subject,
                                                          action, details,
                                                          POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION,
                                                          NULL, &error);
      if (result == NULL)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Authorization error: %s", error->message);
          return FALSE;
        }

      authorized = polkit_authorization_result_get_is_authorized (result);
    }
  else if (g_strcmp0 (method_name, "DeployAppstream") == 0)
    {
      const char *arch, *origin;

      g_variant_get_child (parameters, 1, "&s", &origin);
      g_variant_get_child (parameters, 2, "&s", &arch);

      action = "org.freedesktop.Flatpak.appstream-update";

      details = polkit_details_new ();
      polkit_details_insert (details, "origin", origin);
      polkit_details_insert (details, "arch", arch);

      result = polkit_authority_check_authorization_sync (authority, subject,
                                                          action, details,
                                                          POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION,
                                                          NULL, &error);
      if (result == NULL)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Authorization error: %s", error->message);
          return FALSE;
        }

      authorized = polkit_authorization_result_get_is_authorized (result);
    }
  else if (g_strcmp0 (method_name, "Uninstall") == 0)
    {
      const char *ref;
      gboolean is_app;

      g_variant_get_child (parameters, 1, "&s", &ref);

      is_app = g_str_has_prefix (ref, "app/");
      if (is_app)
        action = "org.freedesktop.Flatpak.app-uninstall";
      else
        action = "org.freedesktop.Flatpak.runtime-uninstall";

      details = polkit_details_new ();
      polkit_details_insert (details, "ref", ref);

      result = polkit_authority_check_authorization_sync (authority, subject,
                                                          action, details,
                                                          POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION,
                                                          NULL, &error);
      if (result == NULL)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Authorization error: %s", error->message);
          return FALSE;
        }

      authorized = polkit_authorization_result_get_is_authorized (result);
    }

  if (!authorized)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_IO_ERROR,
                                             G_IO_ERROR_PERMISSION_DENIED,
                                             "Operation not permitted");
    }

  return authorized;
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  GError *error = NULL;

  g_debug ("Bus aquired, creating skeleton");

  helper = flatpak_system_helper_skeleton_new ();

  g_object_set_data_full (G_OBJECT(helper), "track-alive", GINT_TO_POINTER(42), skeleton_died_cb);

  g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (helper),
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);

  g_signal_connect (helper, "handle-deploy", G_CALLBACK (handle_deploy), NULL);
  g_signal_connect (helper, "handle-deploy-appstream", G_CALLBACK (handle_deploy_appstream), NULL);
  g_signal_connect (helper, "handle-uninstall", G_CALLBACK (handle_uninstall), NULL);

  g_signal_connect (helper, "g-authorize-method",
                    G_CALLBACK (flatpak_authorize_method_handler),
                    NULL);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (helper),
                                         connection,
                                         "/org/freedesktop/Flatpak/SystemHelper",
                                         &error))
    {
      g_warning ("error: %s\n", error->message);
      g_error_free (error);
    }
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
  g_debug ("Name aquired");
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
  g_debug ("Name lost");
  unref_skeleton_in_timeout ();
}

static void
binary_file_changed_cb (GFileMonitor *file_monitor,
                        GFile *file,
                        GFile *other_file,
                        GFileMonitorEvent event_type,
                        gpointer data)
{
  static gboolean got_it = FALSE;

  if (!got_it)
    {
      g_debug ("binary file changed");
      unref_skeleton_in_timeout ();
    }

  got_it = TRUE;
}

static void
message_handler (const gchar   *log_domain,
                 GLogLevelFlags log_level,
                 const gchar   *message,
                 gpointer       user_data)
{
  /* Make this look like normal console output */
  if (log_level & G_LOG_LEVEL_DEBUG)
    g_printerr ("XA: %s\n", message);
  else
    g_printerr ("%s: %s\n", g_get_prgname (), message);
}

int
main (int    argc,
      char **argv)
{
  gchar exe_path[PATH_MAX+1];
  ssize_t exe_path_len;
  gboolean replace;
  gboolean verbose;
  gboolean show_version;
  GBusNameOwnerFlags flags;
  GOptionContext *context;
  g_autoptr(GError) error = NULL;
  const GOptionEntry options[] = {
    { "replace", 'r', 0, G_OPTION_ARG_NONE, &replace,  "Replace old daemon.", NULL },
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose,  "Enable debug output.", NULL },
    { "version", 0, 0, G_OPTION_ARG_NONE, &show_version, "Show program version.", NULL},
    { NULL }
  };

  setlocale (LC_ALL, "");

  g_setenv ("GIO_USE_VFS", "local", TRUE);

  g_set_prgname (argv[0]);

  g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE, message_handler, NULL);

  context = g_option_context_new ("");

  g_option_context_set_summary (context, "Flatpak system helper");
  g_option_context_add_main_entries (context, options, GETTEXT_PACKAGE);

  replace = FALSE;
  verbose = FALSE;
  show_version = FALSE;

  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("%s: %s", g_get_application_name(), error->message);
      g_printerr ("\n");
      g_printerr ("Try \"%s --help\" for more information.",
                  g_get_prgname ());
      g_printerr ("\n");
      g_option_context_free (context);
      return 1;
    }

  if (show_version)
    {
      g_print (PACKAGE_STRING "\n");
      return 0;
    }

  if (verbose)
    g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, message_handler, NULL);

  authority = polkit_authority_get_sync (NULL, &error);
  if (authority == NULL)
    {
      g_printerr ("Can't get polkit authority: %s\n", error->message);
      return 1;
    }

  exe_path_len = readlink ("/proc/self/exe", exe_path, sizeof (exe_path) - 1);
  if (exe_path_len > 0)
    {
      exe_path[exe_path_len] = 0;
      GFileMonitor *monitor;
      g_autoptr(GFile) exe = NULL;
      g_autoptr(GError) local_error = NULL;

      exe = g_file_new_for_path (exe_path);
      monitor =  g_file_monitor_file (exe,
                                      G_FILE_MONITOR_NONE,
                                      NULL,
                                      &local_error);
      if (monitor == NULL)
        g_warning ("Failed to set watch on %s: %s", exe_path, error->message);
      else
        g_signal_connect (monitor, "changed",
                          G_CALLBACK (binary_file_changed_cb), NULL);
    }

  flags = G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;
  if (replace)
    flags |= G_BUS_NAME_OWNER_FLAGS_REPLACE;

  name_owner_id = g_bus_own_name (G_BUS_TYPE_SYSTEM,
                                  "org.freedesktop.Flatpak.SystemHelper",
                                  flags,
                                  on_bus_acquired,
                                  on_name_acquired,
                                  on_name_lost,
                                  NULL,
                                  NULL);

  /* Ensure we don't idle exit */
  schedule_idle_callback ();

  main_loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (main_loop);

  return 0;
}