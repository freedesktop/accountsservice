/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2009-2010 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Written by: Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include <stdlib.h>
#include <stdarg.h>
#include <locale.h>
#include <libintl.h>
#include <syslog.h>
#include <sys/stat.h>
#include <errno.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <glib-unix.h>

#include "daemon.h"

#define NAME_TO_CLAIM "org.freedesktop.Accounts"

static gboolean
ensure_directory (const char  *path,
                  GError     **error)
{
        if (g_mkdir_with_parents (path, 0775) < 0) {
                g_set_error (error,
                             G_FILE_ERROR,
                             g_file_error_from_errno (errno),
                             "Failed to create directory %s: %m",
                             path);
                return FALSE;
        }
        return TRUE;
}

static void
on_bus_acquired (GDBusConnection  *connection,
                 const gchar      *name,
                 gpointer          user_data)
{
        GMainLoop *loop = user_data;
        Daemon *daemon;
        g_autoptr(GError) error = NULL;

        if (!ensure_directory (ICONDIR, &error) ||
            !ensure_directory (USERDIR, &error)) {
                g_printerr ("%s\n", error->message);
                g_main_loop_quit (loop);
                return;
        }

        daemon = daemon_new ();
        if (daemon == NULL) {
                g_printerr ("Failed to initialize daemon\n");
                g_main_loop_quit (loop);
                return;
        }

        openlog ("accounts-daemon", LOG_PID, LOG_DAEMON);
        syslog (LOG_INFO, "started daemon version %s", VERSION);
        closelog ();
        openlog ("accounts-daemon", 0, LOG_AUTHPRIV);
}

static void
on_name_lost (GDBusConnection  *connection,
              const gchar      *name,
              gpointer          user_data)
{
        GMainLoop *loop = user_data;

        g_debug ("got NameLost, exiting");
        g_main_loop_quit (loop);
}

static gboolean debug;

static void
on_log_debug (const gchar *log_domain,
              GLogLevelFlags log_level,
              const gchar *message,
              gpointer user_data)
{
        g_autoptr(GString) string = NULL;
        const gchar *progname;
        int ret G_GNUC_UNUSED;

        string = g_string_new (NULL);

        progname = g_get_prgname ();
        g_string_append_printf (string, "(%s:%lu): %s%sDEBUG: %s\n",
                                progname ? progname : "process", (gulong)getpid (),
                                log_domain ? log_domain : "", log_domain ? "-" : "",
                                message ? message : "(NULL) message");

        ret = write (1, string->str, string->len);
}

static void
log_handler (const gchar   *domain,
             GLogLevelFlags level,
             const gchar   *message,
             gpointer       data)
{
        /* filter out DEBUG messages if debug isn't set */
        if ((level & G_LOG_LEVEL_MASK) == G_LOG_LEVEL_DEBUG && !debug)
                return;

        g_log_default_handler (domain, level, message, data);
}

static gboolean
on_signal_quit (gpointer data)
{
        GMainLoop *loop = data;

        g_main_loop_quit (loop);
        return FALSE;
}

int
main (int argc, char *argv[])
{
        g_autoptr(GMainLoop) loop = NULL;
        g_autoptr(GError) error = NULL;
        GBusNameOwnerFlags flags;
        g_autoptr(GOptionContext) context = NULL;
        static gboolean replace;
        static gboolean show_version;
        static GOptionEntry entries[] = {
                { "version", 0, 0, G_OPTION_ARG_NONE, &show_version, N_("Output version information and exit"), NULL },
                { "replace", 0, 0, G_OPTION_ARG_NONE, &replace, N_("Replace existing instance"), NULL },
                { "debug", 0, 0, G_OPTION_ARG_NONE, &debug, N_("Enable debugging code"), NULL },

                { NULL }
        };

        setlocale (LC_ALL, "");
        bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

#if !GLIB_CHECK_VERSION (2, 35, 3)
        g_type_init ();
#endif

        if (!g_setenv ("GIO_USE_VFS", "local", TRUE)) {
                g_warning ("Couldn't set GIO_USE_GVFS");
                return EXIT_FAILURE;
        }

        context = g_option_context_new ("");
        g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);
        g_option_context_set_summary (context, _("Provides D-Bus interfaces for querying and manipulating\nuser account information."));
        g_option_context_add_main_entries (context, entries, NULL);
        if (!g_option_context_parse (context, &argc, &argv, &error)) {
                g_warning ("%s", error->message);
                return EXIT_FAILURE;
        }

        if (show_version) {
                g_print ("accounts-daemon " VERSION "\n");
                return EXIT_SUCCESS;
        }

        /* If --debug, then print debug messages even when no G_MESSAGES_DEBUG */
        if (debug && !g_getenv ("G_MESSAGES_DEBUG"))
                g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, on_log_debug, NULL);
        g_log_set_default_handler (log_handler, NULL);

        loop = g_main_loop_new (NULL, FALSE);

        flags = G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;
        if (replace)
                flags |= G_BUS_NAME_OWNER_FLAGS_REPLACE;
        g_bus_own_name (G_BUS_TYPE_SYSTEM,
                        NAME_TO_CLAIM,
                        flags,
                        on_bus_acquired,
                        NULL,
                        on_name_lost,
                        loop,
                        NULL);

        g_unix_signal_add (SIGINT, on_signal_quit, loop);
        g_unix_signal_add (SIGTERM, on_signal_quit, loop);

        g_debug ("entering main loop");
        g_main_loop_run (loop);

        g_debug ("exiting");

        return EXIT_SUCCESS;
}

