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

#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <grp.h>

#include <syslog.h>

#include <polkit/polkit.h>

#include "util.h"

static gchar *
get_cmdline_of_pid (GPid pid)
{
        gchar *ret;
        g_autofree gchar *filename = NULL;
        g_autofree gchar *contents = NULL;
        gsize contents_len;
        g_autoptr(GError) error = NULL;
        guint n;

        filename = g_strdup_printf ("/proc/%d/cmdline", (int) pid);

        if (!g_file_get_contents (filename,
                                  &contents,
                                  &contents_len,
                                  &error)) {
                g_warning ("Error opening `%s': %s",
                           filename,
                           error->message);
                return NULL;
        }
        /* The kernel uses '\0' to separate arguments - replace those with a space. */
        for (n = 0; n < contents_len - 1; n++) {
                if (contents[n] == '\0')
                        contents[n] = ' ';
        }

        ret = g_strdup (contents);
        g_strstrip (ret);
        return ret;
}

static gboolean
get_caller_pid (GDBusMethodInvocation *context,
                GPid                  *pid)
{
        g_autoptr(GVariant) reply = NULL;
        g_autoptr(GError) error = NULL;
        guint32 pid_as_int;

        reply = g_dbus_connection_call_sync (g_dbus_method_invocation_get_connection (context),
                                             "org.freedesktop.DBus",
                                             "/org/freedesktop/DBus",
                                             "org.freedesktop.DBus",
                                             "GetConnectionUnixProcessID",
                                             g_variant_new ("(s)",
                                                            g_dbus_method_invocation_get_sender (context)),
                                             G_VARIANT_TYPE ("(u)"),
                                             G_DBUS_CALL_FLAGS_NONE,
                                             -1,
                                             NULL,
                                             &error);

        if (reply == NULL) {
                g_warning ("Could not talk to message bus to find uid of sender %s: %s",
                           g_dbus_method_invocation_get_sender (context),
                           error->message);
                return FALSE;
        }

        g_variant_get (reply, "(u)", &pid_as_int);
        *pid = pid_as_int;

        return TRUE;
}

void
sys_log (GDBusMethodInvocation *context,
         const gchar           *format,
                                ...)
{
        va_list args;
        g_autofree gchar *msg = NULL;

        va_start (args, format);
        msg = g_strdup_vprintf (format, args);
        va_end (args);

        if (context) {
                PolkitSubject *subject;
                g_autofree gchar *cmdline = NULL;
                g_autofree gchar *id = NULL;
                GPid pid = 0;
                gint uid = -1;
                g_autofree gchar *tmp = NULL;

                subject = polkit_system_bus_name_new (g_dbus_method_invocation_get_sender (context));
                id = polkit_subject_to_string (subject);

                if (get_caller_pid (context, &pid)) {
                        cmdline = get_cmdline_of_pid (pid);
                } else {
                        pid = 0;
                        cmdline = NULL;
                }

                if (cmdline != NULL) {
                        if (get_caller_uid (context, &uid)) {
                                tmp = g_strdup_printf ("request by %s [%s pid:%d uid:%d]: %s", id, cmdline, (int) pid, uid, msg);
                        } else {
                                tmp = g_strdup_printf ("request by %s [%s pid:%d]: %s", id, cmdline, (int) pid, msg);
                        }
                } else {
                        if (get_caller_uid (context, &uid) && pid != 0) {
                                tmp = g_strdup_printf ("request by %s [pid:%d uid:%d]: %s", id, (int) pid, uid, msg);
                        } else if (pid != 0) {
                                tmp = g_strdup_printf ("request by %s [pid:%d]: %s", id, (int) pid, msg);
                        } else {
                                tmp = g_strdup_printf ("request by %s: %s", id, msg);
                        }
                }

                g_free (msg);
                msg = g_steal_pointer (&tmp);

                g_object_unref (subject);
        }

        syslog (LOG_NOTICE, "%s", msg);
}

static void
get_caller_loginuid (GDBusMethodInvocation *context, gchar *loginuid, gint size)
{
        GPid pid;
        g_autofree gchar *path = NULL;
        g_autofree gchar *buf = NULL;

        if (get_caller_pid (context, &pid)) {
                path = g_strdup_printf ("/proc/%d/loginuid", (int) pid);
        } else {
                path = NULL;
        }

        if (path != NULL && g_file_get_contents (path, &buf, NULL, NULL)) {
                strncpy (loginuid, buf, size);
        }
        else {
                gint uid;

                if (!get_caller_uid (context, &uid)) {
                        uid = getuid ();
                }
                g_snprintf (loginuid, size, "%d", uid);
        }
}

static gboolean
compat_check_exit_status (int      estatus,
                          GError **error)
{
#if GLIB_CHECK_VERSION(2, 33, 12)
        return g_spawn_check_exit_status (estatus, error);
#else
        if (!WIFEXITED (estatus)) {
                g_set_error (error,
                             G_SPAWN_ERROR,
                             G_SPAWN_ERROR_FAILED,
                             "Exited abnormally");
                return FALSE;
        }
        if (WEXITSTATUS (estatus) != 0) {
                g_set_error (error,
                             G_SPAWN_ERROR,
                             G_SPAWN_ERROR_FAILED,
                             "Exited with code %d",
                             WEXITSTATUS(estatus));
                return FALSE;
        }
        return TRUE;
#endif
}

static void
setup_loginuid (gpointer data)
{
        const char *id = data;
        int fd;

        fd = open ("/proc/self/loginuid", O_WRONLY);
        write (fd, id, strlen (id));
        close (fd);
}

gboolean
spawn_with_login_uid (GDBusMethodInvocation  *context,
                      const gchar            *argv[],
                      GError                **error)
{
        gboolean ret = FALSE;
        gchar loginuid[20];
        gint status;

        get_caller_loginuid (context, loginuid, G_N_ELEMENTS (loginuid));

        if (!g_spawn_sync (NULL, (gchar**)argv, NULL, 0, setup_loginuid, loginuid, NULL, NULL, &status, error))
                goto out;
        if (!compat_check_exit_status (status, error))
                goto out;

        ret = TRUE;
 out:
        return ret;
}

gint
get_user_groups (const gchar  *user,
                 gid_t         group,
                 gid_t       **groups)
{
        gint res;
        gint ngroups;

        ngroups = 0;
        res = getgrouplist (user, group, NULL, &ngroups);

        g_debug ("user %s has %d groups", user, ngroups);
        *groups = g_new (gid_t, ngroups);
        res = getgrouplist (user, group, *groups, &ngroups);

        return res;
}

/**
 * get_admin_groups:
 * @admin_gid_out: (out caller-allocates) (optional): return location for the ID
 *    of the main admin group
 * @groups_out: (out callee-allocates) (transfer container) (optional) (length=n_groups_out):
 *    return location for an array of the extra admin group IDs
 * @n_groups_out: (out caller-allocates) (optional): return location for the
 *    number of elements in @group_out
 *
 * Get the GIDs of the admin groups on the system, as set at configure time for
 * accountsservice. The main admin group ID (typically for the `sudo` or `wheel`
 * group) will be returned in @admin_gid_out. Any group IDs for other admin
 * groups (such as `lpadmin` or `systemd-journal`) will be returned in
 * @groups_out, which should be freed by the caller using g_free().
 *
 * Returns: %TRUE on success, %FALSE if one or more of the groups could not be
 *    looked up
 */
gboolean
get_admin_groups (gid_t  *admin_gid_out,
                  gid_t **groups_out,
                  gsize  *n_groups_out)
{
        g_auto(GStrv) extra_admin_groups = NULL;
        g_autofree gid_t *extra_admin_groups_gids = NULL;
        gsize n_extra_admin_groups_gids = 0;
        gsize i;
        gboolean retval = FALSE;
        struct group *grp;
        gid_t admin_gid = 0;

        /* Get the main admin group ID. */
        grp = getgrnam (ADMIN_GROUP);
        if (grp == NULL)
                goto out;
        admin_gid = grp->gr_gid;

        /* Get the extra admin group IDs. */
        extra_admin_groups = g_strsplit (EXTRA_ADMIN_GROUPS, ",", 0);
        n_extra_admin_groups_gids = 0;
        extra_admin_groups_gids = g_new0 (gid_t, g_strv_length (extra_admin_groups));

        for (i = 0; extra_admin_groups[i] != NULL; i++) {
                struct group *extra_group;
                extra_group = getgrnam (extra_admin_groups[i]);
                if (extra_group == NULL)
                        goto out;
                if (extra_group->gr_gid == admin_gid)
                        continue;

                extra_admin_groups_gids[n_extra_admin_groups_gids++] = extra_group->gr_gid;
        }

        retval = TRUE;

out:
        if (!retval) {
                admin_gid = 0;
                g_clear_pointer (&extra_admin_groups_gids, g_free);
                n_extra_admin_groups_gids = 0;
        }

        if (admin_gid_out != NULL)
                *admin_gid_out = admin_gid;
        if (groups_out != NULL)
                *groups_out = g_steal_pointer (&extra_admin_groups_gids);
        if (n_groups_out != NULL)
                *n_groups_out = n_extra_admin_groups_gids;

        return retval;
}

gboolean
get_caller_uid (GDBusMethodInvocation *context,
                gint                  *uid)
{
        g_autoptr(GVariant) reply = NULL;
        g_autoptr(GError) error = NULL;

        reply = g_dbus_connection_call_sync (g_dbus_method_invocation_get_connection (context),
                                             "org.freedesktop.DBus",
                                             "/org/freedesktop/DBus",
                                             "org.freedesktop.DBus",
                                             "GetConnectionUnixUser",
                                             g_variant_new ("(s)",
                                                            g_dbus_method_invocation_get_sender (context)),
                                             G_VARIANT_TYPE ("(u)"),
                                             G_DBUS_CALL_FLAGS_NONE,
                                             -1,
                                             NULL,
                                             &error);

        if (reply == NULL) {
                g_warning ("Could not talk to message bus to find uid of sender %s: %s",
                           g_dbus_method_invocation_get_sender (context),
                           error->message);
                return FALSE;
        }

        g_variant_get (reply, "(u)", uid);

        return TRUE;
}
