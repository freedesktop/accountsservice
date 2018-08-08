/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
  *
  * Copyright (C) 2004-2005 James M. Cape <jcape@ignore-your.tv>.
  * Copyright (C) 2007-2008 William Jon McCann <mccann@jhu.edu>
  * Copyright (C) 2009-2010 Red Hat, Inc.
  * Copyright © 2013 Canonical Limited
  *
  * This program is free software; you can redistribute it and/or modify
  * it under the terms of the GNU General Public License as published by
  * the Free Software Foundation; either version 2 of the License, or
  * (at your option) any later version.
  *
  * This program is distributed in the hope that it will be useful,
  * but WITHOUT ANY WARRANTY; without even the implied warranty of
  * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  * GNU General Public License for more details.
  *
  * You should have received a copy of the GNU General Public License
  * along with this program; if not, write to the Free Software
  * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
  */

#define _BSD_SOURCE

#include "config.h"

#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <grp.h>
#ifdef HAVE_SHADOW_H
#include <shadow.h>
#endif

#include <glib.h>
#include <glib/gi18n.h>
#include <glib-object.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <polkit/polkit.h>

#include "user-classify.h"
#include "daemon.h"
#include "user.h"
#include "accounts-user-generated.h"
#include "util.h"

struct User {
        AccountsUserSkeleton parent;

        GDBusConnection *system_bus_connection;
        gchar *object_path;

        Daemon       *daemon;

        GKeyFile     *keyfile;

        gid_t         gid;
        gint64        expiration_time;
        gint64        last_change_time;
        gint64        min_days_between_changes;
        gint64        max_days_between_changes;
        gint64        days_to_warn;
        gint64        days_after_expiration_until_lock;
        GVariant     *login_history;
        gchar        *icon_file;
        gchar        *default_icon_file;
        gboolean      account_expiration_policy_known;
        gboolean      cached;

        guint        *extension_ids;
        guint         n_extension_ids;

        guint         changed_timeout_id;
};

typedef struct UserClass
{
        AccountsUserSkeletonClass parent_class;
} UserClass;

static void user_accounts_user_iface_init (AccountsUserIface *iface);

G_DEFINE_TYPE_WITH_CODE (User, user, ACCOUNTS_TYPE_USER_SKELETON, G_IMPLEMENT_INTERFACE (ACCOUNTS_TYPE_USER, user_accounts_user_iface_init));

static gint
account_type_from_pwent (struct passwd *pwent)
{
        struct group *grp;
        gint i;

        if (pwent->pw_uid == 0) {
                g_debug ("user is root so account type is administrator");
                return ACCOUNT_TYPE_ADMINISTRATOR;
        }

        grp = getgrnam (ADMIN_GROUP);
        if (grp == NULL) {
                g_debug (ADMIN_GROUP " group not found");
                return ACCOUNT_TYPE_STANDARD;
        }

        for (i = 0; grp->gr_mem[i] != NULL; i++) {
                if (g_strcmp0 (grp->gr_mem[i], pwent->pw_name) == 0) {
                        return ACCOUNT_TYPE_ADMINISTRATOR;
                }
        }

        return ACCOUNT_TYPE_STANDARD;
}

static void
user_reset_icon_file (User *user)
{
        const char *icon_file;
        gboolean    icon_is_default;
        const char *home_dir;

        icon_file = accounts_user_get_icon_file (ACCOUNTS_USER (user));

        if (icon_file == NULL || g_strcmp0 (icon_file, user->default_icon_file) == 0) {
                icon_is_default = TRUE;
        } else {
                icon_is_default = FALSE;
        }

        g_free (user->default_icon_file);
        home_dir = accounts_user_get_home_directory (ACCOUNTS_USER (user));

        user->default_icon_file = g_build_filename (home_dir, ".face", NULL);

        if (icon_is_default) {
                accounts_user_set_icon_file (ACCOUNTS_USER (user), user->default_icon_file);
        }
}

void
user_update_from_pwent (User          *user,
                        struct passwd *pwent,
                        struct spwd   *spent)
{
        g_autofree gchar *real_name = NULL;
        gboolean is_system_account;
        const gchar *passwd;
        gboolean locked;
        PasswordMode mode;
        AccountType account_type;

        g_object_freeze_notify (G_OBJECT (user));

        if (pwent->pw_gecos && pwent->pw_gecos[0] != '\0') {
                gchar *first_comma = NULL;
                gchar *valid_utf8_name = NULL;

                if (g_utf8_validate (pwent->pw_gecos, -1, NULL)) {
                        valid_utf8_name = pwent->pw_gecos;
                        first_comma = g_utf8_strchr (valid_utf8_name, -1, ',');
                }
                else {
                        g_warning ("User %s has invalid UTF-8 in GECOS field. "
                                   "It would be a good thing to check /etc/passwd.",
                                   pwent->pw_name ? pwent->pw_name : "");
                }

                if (first_comma) {
                        real_name = g_strndup (valid_utf8_name,
                                                  (first_comma - valid_utf8_name));
                }
                else if (valid_utf8_name) {
                        real_name = g_strdup (valid_utf8_name);
                }
                else {
                        real_name = NULL;
                }

                if (real_name && real_name[0] == '\0') {
                        g_clear_pointer (&real_name, g_free);
                }
        }
        else {
                real_name = NULL;
        }

        accounts_user_set_real_name (ACCOUNTS_USER (user), real_name);
        accounts_user_set_uid (ACCOUNTS_USER (user), pwent->pw_uid);

        user->gid = pwent->pw_gid;

        account_type = account_type_from_pwent (pwent);
        accounts_user_set_account_type (ACCOUNTS_USER (user), account_type);

        accounts_user_set_user_name (ACCOUNTS_USER (user), pwent->pw_name);
        accounts_user_set_home_directory (ACCOUNTS_USER (user), pwent->pw_dir);
        user_reset_icon_file (user);

        accounts_user_set_shell (ACCOUNTS_USER (user), pwent->pw_shell);

        passwd = NULL;
        if (spent)
                passwd = spent->sp_pwdp;

        if (passwd && passwd[0] == '!') {
                locked = TRUE;
        }
        else {
                locked = FALSE;
        }

        accounts_user_set_locked (ACCOUNTS_USER (user), locked);

        if (passwd == NULL || passwd[0] != 0) {
                mode = PASSWORD_MODE_REGULAR;
        }
        else {
                mode = PASSWORD_MODE_NONE;
        }

        if (spent) {
                if (spent->sp_lstchg == 0) {
                        mode = PASSWORD_MODE_SET_AT_LOGIN;
                }

                user->expiration_time = spent->sp_expire;
                user->last_change_time  = spent->sp_lstchg;
                user->min_days_between_changes = spent->sp_min;
                user->max_days_between_changes = spent->sp_max;
                user->days_to_warn  = spent->sp_warn;
                user->days_after_expiration_until_lock = spent->sp_inact;
                user->account_expiration_policy_known = TRUE;
        }

        accounts_user_set_password_mode (ACCOUNTS_USER (user), mode);
        is_system_account = !user_classify_is_human (accounts_user_get_uid (ACCOUNTS_USER (user)),
                                                     accounts_user_get_user_name (ACCOUNTS_USER (user)),
                                                     accounts_user_get_shell (ACCOUNTS_USER (user)),
                                                     passwd);
        accounts_user_set_system_account (ACCOUNTS_USER (user), is_system_account);

        g_object_thaw_notify (G_OBJECT (user));
}

void
user_update_from_keyfile (User     *user,
                          GKeyFile *keyfile)
{
        gchar *s;

        g_object_freeze_notify (G_OBJECT (user));

        s = g_key_file_get_string (keyfile, "User", "Language", NULL);
        if (s != NULL) {
                accounts_user_set_language (ACCOUNTS_USER (user), s);
                g_clear_pointer (&s, g_free);
        }

        s = g_key_file_get_string (keyfile, "User", "XSession", NULL);
        if (s != NULL) {
                accounts_user_set_xsession (ACCOUNTS_USER (user), s);

                /* for backward compat */
                accounts_user_set_session (ACCOUNTS_USER (user), s);
                g_clear_pointer (&s, g_free);
        }

        s = g_key_file_get_string (keyfile, "User", "Session", NULL);
        if (s != NULL) {
                accounts_user_set_session (ACCOUNTS_USER (user), s);
                g_clear_pointer (&s, g_free);
        }

        s = g_key_file_get_string (keyfile, "User", "SessionType", NULL);
        if (s != NULL) {
                accounts_user_set_session_type (ACCOUNTS_USER (user), s);
                g_clear_pointer (&s, g_free);
        }

        s = g_key_file_get_string (keyfile, "User", "Email", NULL);
        if (s != NULL) {
                accounts_user_set_email (ACCOUNTS_USER (user), s);
                g_clear_pointer (&s, g_free);
        }

        s = g_key_file_get_string (keyfile, "User", "Location", NULL);
        if (s != NULL) {
                accounts_user_set_location (ACCOUNTS_USER (user), s);
                g_clear_pointer (&s, g_free);
        }

        s = g_key_file_get_string (keyfile, "User", "PasswordHint", NULL);
        if (s != NULL) {
                accounts_user_set_password_hint (ACCOUNTS_USER (user), s);
                g_clear_pointer (&s, g_free);
        }

        s = g_key_file_get_string (keyfile, "User", "Icon", NULL);
        if (s != NULL) {
                accounts_user_set_icon_file (ACCOUNTS_USER (user), s);
                g_clear_pointer (&s, g_free);
        }

        if (g_key_file_has_key (keyfile, "User", "SystemAccount", NULL)) {
            gboolean system_account;

            system_account = g_key_file_get_boolean (keyfile, "User", "SystemAccount", NULL);
            accounts_user_set_system_account (ACCOUNTS_USER (user), system_account);
        }

        g_clear_pointer (&user->keyfile, g_key_file_unref);
        user->keyfile = g_key_file_ref (keyfile);
        user_set_cached (user, TRUE);

        g_object_thaw_notify (G_OBJECT (user));
}

void
user_update_local_account_property (User          *user,
                                    gboolean       local)
{
        accounts_user_set_local_account (ACCOUNTS_USER (user), local);
}

void
user_update_system_account_property (User          *user,
                                     gboolean       system)
{
        accounts_user_set_system_account (ACCOUNTS_USER (user), system);
}

static void
user_save_to_keyfile (User     *user,
                      GKeyFile *keyfile)
{
        g_key_file_remove_group (keyfile, "User", NULL);

        if (accounts_user_get_email (ACCOUNTS_USER (user)))
                g_key_file_set_string (keyfile, "User", "Email", accounts_user_get_email (ACCOUNTS_USER (user)));

        if (accounts_user_get_language (ACCOUNTS_USER (user)))
                g_key_file_set_string (keyfile, "User", "Language", accounts_user_get_language (ACCOUNTS_USER (user)));

        if (accounts_user_get_session (ACCOUNTS_USER (user)))
                g_key_file_set_string (keyfile, "User", "Session", accounts_user_get_session (ACCOUNTS_USER (user)));

        if (accounts_user_get_session_type (ACCOUNTS_USER (user)))
                g_key_file_set_string (keyfile, "User", "SessionType", accounts_user_get_session_type (ACCOUNTS_USER (user)));

        if (accounts_user_get_xsession (ACCOUNTS_USER (user)))
                g_key_file_set_string (keyfile, "User", "XSession", accounts_user_get_xsession (ACCOUNTS_USER (user)));

        if (accounts_user_get_location (ACCOUNTS_USER (user)))
                g_key_file_set_string (keyfile, "User", "Location", accounts_user_get_location (ACCOUNTS_USER (user)));

        if (accounts_user_get_password_hint (ACCOUNTS_USER (user)))
                g_key_file_set_string (keyfile, "User", "PasswordHint", accounts_user_get_password_hint (ACCOUNTS_USER (user)));

        if (accounts_user_get_icon_file (ACCOUNTS_USER (user)))
                g_key_file_set_string (keyfile, "User", "Icon", accounts_user_get_icon_file (ACCOUNTS_USER (user)));

        g_key_file_set_boolean (keyfile, "User", "SystemAccount", accounts_user_get_system_account (ACCOUNTS_USER (user)));

        user_set_cached (user, TRUE);
}

static void
save_extra_data (User *user)
{
        g_autofree gchar *data = NULL;
        g_autofree gchar *filename = NULL;
        g_autoptr(GError) error = NULL;

        user_save_to_keyfile (user, user->keyfile);

        data = g_key_file_to_data (user->keyfile, NULL, &error);
        if (data == NULL) {
                g_warning ("Saving data for user %s failed: %s",
                           accounts_user_get_user_name (ACCOUNTS_USER (user)), error->message);
                return;
        }

        filename = g_build_filename (USERDIR,
                                     accounts_user_get_user_name (ACCOUNTS_USER (user)),
                                     NULL);
        g_file_set_contents (filename, data, -1, &error);
}

static void
move_extra_data (const gchar *old_name,
                 const gchar *new_name)
{
        g_autofree gchar *old_filename = NULL;
        g_autofree gchar *new_filename = NULL;

        old_filename = g_build_filename (USERDIR,
                                         old_name, NULL);
        new_filename = g_build_filename (USERDIR,
                                         new_name, NULL);

        g_rename (old_filename, new_filename);
}

static GVariant *
user_extension_get_value (User                    *user,
                          GDBusInterfaceInfo      *interface,
                          const GDBusPropertyInfo *property)
{
        const GVariantType *type = G_VARIANT_TYPE (property->signature);
        GVariant *value;
        g_autofree gchar *printed = NULL;
        gint i;

        /* First, try to get the value from the keyfile */
        printed = g_key_file_get_value (user->keyfile, interface->name, property->name, NULL);
        if (printed) {
                value = g_variant_parse (type, printed, NULL, NULL, NULL);
                if (value != NULL)
                        return value;
        }

        /* If that didn't work, try for a default value annotation */
        for (i = 0; property->annotations && property->annotations[i]; i++) {
                GDBusAnnotationInfo *annotation = property->annotations[i];

                if (g_str_equal (annotation->key, "org.freedesktop.Accounts.DefaultValue.String")) {
                        if (g_str_equal (property->signature, "s"))
                                return g_variant_ref_sink (g_variant_new_string (annotation->value));
                }
                else if (g_str_equal (annotation->key, "org.freedesktop.Accounts.DefaultValue")) {
                        value = g_variant_parse (type, annotation->value, NULL, NULL, NULL);
                        if (value != NULL)
                                return value;
                }
        }

        /* Nothing found... */
        return NULL;
}

static void
user_extension_get_property (User                  *user,
                             Daemon                *daemon,
                             GDBusInterfaceInfo    *interface,
                             GDBusMethodInvocation *invocation)
{
        const GDBusPropertyInfo *property = g_dbus_method_invocation_get_property_info (invocation);
        g_autoptr(GVariant) value = NULL;

        value = user_extension_get_value (user, interface, property);

        if (value) {
                g_dbus_method_invocation_return_value (invocation, g_variant_new ("(v)", value));
        }
        else {
                g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                       "Key '%s' is not set and has no default value",
                                                       property->name);
        }
}

static void
user_extension_get_all_properties (User                  *user,
                                   Daemon                *daemon,
                                   GDBusInterfaceInfo    *interface,
                                   GDBusMethodInvocation *invocation)
{
        GVariantBuilder builder;
        gint i;

        g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
        for (i = 0; interface->properties && interface->properties[i]; i++) {
                GDBusPropertyInfo *property = interface->properties[i];
                g_autoptr(GVariant) value = NULL;

                value = user_extension_get_value (user, interface, property);

                if (value)
                        g_variant_builder_add (&builder, "{sv}", property->name, value);
        }

        g_dbus_method_invocation_return_value (invocation, g_variant_new ("(a{sv})", &builder));
}

static void
user_extension_set_property (User                  *user,
                             Daemon                *daemon,
                             GDBusInterfaceInfo    *interface,
                             GDBusMethodInvocation *invocation)
{
        const GDBusPropertyInfo *property = g_dbus_method_invocation_get_property_info (invocation);
        g_autoptr(GVariant) value = NULL;
        g_autofree gchar *printed = NULL;
        g_autofree gchar *prev = NULL;

        g_variant_get_child (g_dbus_method_invocation_get_parameters (invocation), 2, "v", &value);

        /* We'll always have the type when we parse it back so
         * we don't need it to be printed with annotations.
         */
        printed = g_variant_print (value, FALSE);

        /* May as well try to avoid the thrashing... */
        prev = g_key_file_get_value (user->keyfile, interface->name, property->name, NULL);

        if (!prev || !g_str_equal (printed, prev)) {
                g_key_file_set_value (user->keyfile, interface->name, property->name, printed);

                /* Emit a change signal.  Use invalidation
                 * because the data may not be world-readable.
                 */
                g_dbus_connection_emit_signal (g_dbus_method_invocation_get_connection (invocation),
                                               NULL, /* destination_bus_name */
                                               g_dbus_method_invocation_get_object_path (invocation),
                                               "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                               g_variant_new_parsed ("( %s, %a{sv}, [ %s ] )",
                                                                     interface->name, NULL, property->name),
                                               NULL);

                accounts_user_emit_changed (ACCOUNTS_USER (user));
                save_extra_data (user);
        }

        g_dbus_method_invocation_return_value (invocation, g_variant_new ("()"));
}

static void
user_extension_authentication_done (Daemon                *daemon,
                                    User                  *user,
                                    GDBusMethodInvocation *invocation,
                                    gpointer               user_data)
{
        GDBusInterfaceInfo *interface = user_data;
        const gchar *method_name;

        method_name = g_dbus_method_invocation_get_method_name (invocation);

        if (g_str_equal (method_name, "Get"))
                user_extension_get_property (user, daemon, interface, invocation);
        else if (g_str_equal (method_name, "GetAll"))
                user_extension_get_all_properties (user, daemon, interface, invocation);
        else if (g_str_equal (method_name, "Set"))
                user_extension_set_property (user, daemon, interface, invocation);
        else
                g_assert_not_reached ();
}

static void
user_extension_method_call (GDBusConnection       *connection,
                            const gchar           *sender,
                            const gchar           *object_path,
                            const gchar           *interface_name,
                            const gchar           *method_name,
                            GVariant              *parameters,
                            GDBusMethodInvocation *invocation,
                            gpointer               user_data)
{
        User *user = user_data;
        GDBusInterfaceInfo *iface_info;
        const gchar *annotation_name;
        const gchar *action_id;
        gint uid;
        gint i;

        /* We don't allow method calls on extension interfaces, so we
         * should only ever see property calls here.
         */
        g_assert_cmpstr (interface_name, ==, "org.freedesktop.DBus.Properties");

        /* Now get the real interface name */
        g_variant_get_child (parameters, 0, "&s", &interface_name);

        if (get_caller_uid (invocation, &uid) && (uid_t) uid == accounts_user_get_uid (ACCOUNTS_USER (user))) {
                /* Operation on sender's own User object */
                if (g_str_equal (method_name, "Set")) {
                        annotation_name = "org.freedesktop.Accounts.Authentication.ChangeOwn";
                        action_id = "org.freedesktop.accounts.change-own-user-data";
                }
                else {
                        annotation_name = "org.freedesktop.Accounts.Authentication.ReadOwn";
                        action_id = ""; /* reading allowed by default */
                }
        }
        else {
                /* Operation on someone else's User object */
                if (g_str_equal (method_name, "Set")) {
                        annotation_name = "org.freedesktop.Accounts.Authentication.ChangeAny";
                        action_id = "org.freedesktop.accounts.user-administration";
                }
                else {
                        annotation_name = "org.freedesktop.Accounts.Authentication.ReadAny";
                        action_id = ""; /* reading allowed by default */
                }
        }

        iface_info = g_hash_table_lookup (daemon_get_extension_ifaces (user->daemon), interface_name);
        g_assert (iface_info != NULL);

        for (i = 0; iface_info->annotations && iface_info->annotations[i]; i++) {
                if (g_str_equal (iface_info->annotations[i]->key, annotation_name)) {
                        action_id = iface_info->annotations[i]->value;
                        break;
                }
        }

        if (action_id[0] == '\0') {
                /* Should always allow this call, so just do it now */
                user_extension_authentication_done (user->daemon, user, invocation, iface_info);
        }
        else {
                daemon_local_check_auth (user->daemon, user, action_id, TRUE,
                                         user_extension_authentication_done,
                                         invocation, iface_info, NULL);
        }
}

static void
user_register_extensions (User *user)
{
        static const GDBusInterfaceVTable vtable = {
                user_extension_method_call,
                NULL /* get_property */,
                NULL /* set_property */
        };
        GHashTable *extensions;
        GHashTableIter iter;
        gpointer iface;
        gint i = 0;

        g_assert (user->extension_ids == NULL);
        g_assert (user->n_extension_ids == 0);

        extensions = daemon_get_extension_ifaces (user->daemon);
        user->n_extension_ids = g_hash_table_size (extensions);
        user->extension_ids = g_new (guint, user->n_extension_ids);
        g_hash_table_iter_init (&iter, extensions);

        /* Ignore errors when registering more interfaces because (a)
         * they won't happen and (b) even if they do, we still want to
         * publish the main user interface.
         */
        while (g_hash_table_iter_next (&iter, NULL, &iface))
                user->extension_ids[i++] = g_dbus_connection_register_object (user->system_bus_connection,
                                                                              g_dbus_interface_skeleton_get_object_path (G_DBUS_INTERFACE_SKELETON (user)), iface,
                                                                              &vtable, user, NULL, NULL);
}

static gchar *
compute_object_path (User *user)
{
        gchar *object_path;

        object_path = g_strdup_printf ("/org/freedesktop/Accounts/User%lu",
                                       (gulong) accounts_user_get_uid (ACCOUNTS_USER (user)));

        return object_path;
}

static gboolean
on_user_changed_timeout (User *user)
{
        user->changed_timeout_id = 0;
        accounts_user_emit_changed (ACCOUNTS_USER (user));

        return G_SOURCE_REMOVE;
}

static void
on_user_property_notify (User *user)
{
        if (user->changed_timeout_id != 0)
                return;

        user->changed_timeout_id = g_timeout_add (250, (GSourceFunc) on_user_changed_timeout, user);
}

void
user_register (User *user)
{
        g_autoptr(GError) error = NULL;
        g_autofree gchar *object_path = NULL;

        user->system_bus_connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
        if (user->system_bus_connection == NULL) {
                if (error != NULL)
                        g_critical ("error getting system bus: %s", error->message);
                return;
        }

        object_path = compute_object_path (user);

        if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (user),
                                               user->system_bus_connection,
                                               object_path,
                                               &error)) {
                if (error != NULL)
                        g_critical ("error exporting user object: %s", error->message);
                return;
        }

        user_register_extensions (user);

        g_signal_connect (G_OBJECT (user), "notify", G_CALLBACK (on_user_property_notify), NULL);
}

void
user_save (User *user)
{
    save_extra_data (user);
}

void
user_unregister (User *user)
{
        g_signal_handlers_disconnect_by_func (G_OBJECT (user), G_CALLBACK (on_user_property_notify), NULL);

        g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (user));

        if (user->extension_ids) {
                guint i;

                for (i = 0; i < user->n_extension_ids; i++) {
                        /* In theory, if an error happened during registration, we could have 0 here. */
                        if (user->extension_ids[i] == 0)
                                continue;

                        g_dbus_connection_unregister_object (user->system_bus_connection, user->extension_ids[i]);
                }

                g_clear_pointer (&user->extension_ids, g_free);
                user->n_extension_ids = 0;
        }
}

void
user_changed (User *user)
{
        accounts_user_emit_changed (ACCOUNTS_USER (user));
}

User *
user_new (Daemon *daemon,
          uid_t   uid)
{
        User *user;

        user = g_object_new (TYPE_USER, NULL);
        user->daemon = daemon;
        accounts_user_set_uid (ACCOUNTS_USER (user), uid);

        return user;
}

const gchar *
user_get_user_name (User *user)
{
        return accounts_user_get_user_name (ACCOUNTS_USER (user));
}

gboolean
user_get_system_account (User *user)
{
        return accounts_user_get_system_account (ACCOUNTS_USER (user));
}

gboolean
user_get_local_account (User *user)
{
        return accounts_user_get_local_account (ACCOUNTS_USER (user));;
}

const gchar *
user_get_object_path (User *user)
{
        return g_dbus_interface_skeleton_get_object_path (G_DBUS_INTERFACE_SKELETON (user));
}

uid_t
user_get_uid (User *user)
{
        return accounts_user_get_uid (ACCOUNTS_USER (user));
}

const gchar *
user_get_shell(User *user)
{
	return accounts_user_get_shell (ACCOUNTS_USER (user));
}

gboolean
user_get_cached (User *user)
{
        return user->cached;
}

void
user_set_cached (User     *user,
                 gboolean  cached)
{
        user->cached = cached;
}

static void
throw_error (GDBusMethodInvocation *context,
             gint                   error_code,
             const gchar           *format,
             ...)
{
        va_list args;
        g_autofree gchar *message = NULL;

        va_start (args, format);
        message = g_strdup_vprintf (format, args);
        va_end (args);

        g_dbus_method_invocation_return_error (context, ERROR, error_code, "%s", message);
}

static void
user_change_real_name_authorized_cb (Daemon                *daemon,
                                     User                  *user,
                                     GDBusMethodInvocation *context,
                                     gpointer               data)

{
        gchar *name = data;
        g_autoptr(GError) error = NULL;
        const gchar *argv[6];

        if (g_strcmp0 (accounts_user_get_real_name (ACCOUNTS_USER (user)), name) != 0) {
                sys_log (context,
                         "change real name of user '%s' (%d) to '%s'",
                         accounts_user_get_user_name (ACCOUNTS_USER (user)),
                         accounts_user_get_uid (ACCOUNTS_USER (user)),
                         name);

                argv[0] = "/usr/sbin/usermod";
                argv[1] = "-c";
                argv[2] = name;
                argv[3] = "--";
                argv[4] = accounts_user_get_user_name (ACCOUNTS_USER (user));
                argv[5] = NULL;

                if (!spawn_with_login_uid (context, argv, &error)) {
                        throw_error (context, ERROR_FAILED, "running '%s' failed: %s", argv[0], error->message);
                        return;
                }

                accounts_user_set_real_name (ACCOUNTS_USER (user), name);
        }

        accounts_user_complete_set_real_name (ACCOUNTS_USER (user), context);
}

static gboolean
user_set_real_name (AccountsUser          *auser,
                    GDBusMethodInvocation *context,
                    const gchar           *real_name)
{
        User *user = (User*)auser;
        int uid;
        const gchar *action_id;

        if (!get_caller_uid (context, &uid)) {
                throw_error (context, ERROR_FAILED, "identifying caller failed");
                return FALSE;
        }

        if (accounts_user_get_uid (ACCOUNTS_USER (user)) == (uid_t) uid)
                action_id = "org.freedesktop.accounts.change-own-user-data";
        else
                action_id = "org.freedesktop.accounts.user-administration";

        daemon_local_check_auth (user->daemon,
                                 user,
                                 action_id,
                                 TRUE,
                                 user_change_real_name_authorized_cb,
                                 context,
                                 g_strdup (real_name),
                                 (GDestroyNotify)g_free);

        return TRUE;
}

static void
user_change_user_name_authorized_cb (Daemon                *daemon,
                                     User                  *user,
                                     GDBusMethodInvocation *context,
                                     gpointer               data)

{
        gchar *name = data;
        gchar *old_name;
        g_autoptr(GError) error = NULL;
        const gchar *argv[6];

        if (g_strcmp0 (accounts_user_get_user_name (ACCOUNTS_USER (user)), name) != 0) {
                old_name = g_strdup (accounts_user_get_user_name (ACCOUNTS_USER (user)));
                sys_log (context,
                         "change name of user '%s' (%d) to '%s'",
                         old_name,
                         accounts_user_get_uid (ACCOUNTS_USER (user)),
                         name);

                argv[0] = "/usr/sbin/usermod";
                argv[1] = "-l";
                argv[2] = name;
                argv[3] = "--";
                argv[4] = accounts_user_get_user_name (ACCOUNTS_USER (user));
                argv[5] = NULL;

                if (!spawn_with_login_uid (context, argv, &error)) {
                        throw_error (context, ERROR_FAILED, "running '%s' failed: %s", argv[0], error->message);
                        return;
                }

                accounts_user_set_user_name (ACCOUNTS_USER (user), name);

                move_extra_data (old_name, name);
        }

        accounts_user_complete_set_user_name (ACCOUNTS_USER (user), context);
}


static gboolean
user_set_user_name (AccountsUser          *auser,
                    GDBusMethodInvocation *context,
                    const gchar           *user_name)
{
        User *user = (User*)auser;
        daemon_local_check_auth (user->daemon,
                                 user,
                                 "org.freedesktop.accounts.user-administration",
                                 TRUE,
                                 user_change_user_name_authorized_cb,
                                 context,
                                 g_strdup (user_name),
                                 (GDestroyNotify)g_free);

        return TRUE;
}

static void
user_change_email_authorized_cb (Daemon                *daemon,
                                 User                  *user,
                                 GDBusMethodInvocation *context,
                                 gpointer               data)

{
        gchar *email = data;

        if (g_strcmp0 (accounts_user_get_email (ACCOUNTS_USER (user)), email) != 0) {
                accounts_user_set_email (ACCOUNTS_USER (user), email);

                save_extra_data (user);
        }

        accounts_user_complete_set_email (ACCOUNTS_USER (user), context);
}



static gboolean
user_set_email (AccountsUser          *auser,
                GDBusMethodInvocation *context,
                const gchar           *email)
{
        User *user = (User*)auser;
        int uid;
        const gchar *action_id;

        if (!get_caller_uid (context, &uid)) {
                throw_error (context, ERROR_FAILED, "identifying caller failed");
                return FALSE;
        }

        if (accounts_user_get_uid (ACCOUNTS_USER (user)) == (uid_t) uid)
                action_id = "org.freedesktop.accounts.change-own-user-data";
        else
                action_id = "org.freedesktop.accounts.user-administration";

        daemon_local_check_auth (user->daemon,
                                 user,
                                 action_id,
                                 TRUE,
                                 user_change_email_authorized_cb,
                                 context,
                                 g_strdup (email),
                                 (GDestroyNotify)g_free);

        return TRUE;
}

static void
user_change_language_authorized_cb (Daemon                *daemon,
                                    User                  *user,
                                    GDBusMethodInvocation *context,
                                    gpointer               data)

{
        gchar *language = data;

        if (g_strcmp0 (accounts_user_get_language (ACCOUNTS_USER (user)), language) != 0) {
                accounts_user_set_language (ACCOUNTS_USER (user), language);

                save_extra_data (user);
        }

        accounts_user_complete_set_language (ACCOUNTS_USER (user), context);
}



static gboolean
user_set_language (AccountsUser          *auser,
                   GDBusMethodInvocation *context,
                   const gchar           *language)
{
        User *user = (User*)auser;
        int uid;
        const gchar *action_id;

        if (!get_caller_uid (context, &uid)) {
                throw_error (context, ERROR_FAILED, "identifying caller failed");
                return FALSE;
        }

        if (accounts_user_get_uid (ACCOUNTS_USER (user)) == (uid_t) uid)
                action_id = "org.freedesktop.accounts.change-own-user-data";
        else
                action_id = "org.freedesktop.accounts.user-administration";

        daemon_local_check_auth (user->daemon,
                                 user,
                                 action_id,
                                 TRUE,
                                 user_change_language_authorized_cb,
                                 context,
                                 g_strdup (language),
                                 (GDestroyNotify)g_free);

        return TRUE;
}

static void
user_change_session_authorized_cb (Daemon                *daemon,
                                   User                  *user,
                                   GDBusMethodInvocation *context,
                                   gpointer               user_data)

{
        const gchar *session = user_data;

        if (g_strcmp0 (accounts_user_get_session (ACCOUNTS_USER (user)), session) != 0) {
                accounts_user_set_session (ACCOUNTS_USER (user), session);

                save_extra_data (user);
        }

        accounts_user_complete_set_session (ACCOUNTS_USER (user), context);
}

static gboolean
user_set_session (AccountsUser          *auser,
                  GDBusMethodInvocation *context,
                  const gchar           *session)
{
        User *user = (User*)auser;
        int uid;
        const gchar *action_id;

        if (!get_caller_uid (context, &uid)) {
                throw_error (context, ERROR_FAILED, "identifying caller failed");
                return FALSE;
        }

        if (accounts_user_get_uid (ACCOUNTS_USER (user)) == (uid_t) uid)
                action_id = "org.freedesktop.accounts.change-own-user-data";
        else
                action_id = "org.freedesktop.accounts.user-administration";

        daemon_local_check_auth (user->daemon,
                                 user,
                                 action_id,
                                 TRUE,
                                 user_change_session_authorized_cb,
                                 context,
                                 g_strdup (session),
                                 (GDestroyNotify) g_free);

        return TRUE;
}

static void
user_change_session_type_authorized_cb (Daemon                *daemon,
                                        User                  *user,
                                        GDBusMethodInvocation *context,
                                        gpointer               user_data)

{
        const gchar *session_type = user_data;

        if (g_strcmp0 (accounts_user_get_session_type (ACCOUNTS_USER (user)), session_type) != 0) {
                accounts_user_set_session_type (ACCOUNTS_USER (user), session_type);

                save_extra_data (user);
        }

        accounts_user_complete_set_session_type (ACCOUNTS_USER (user), context);
}

static gboolean
user_set_session_type (AccountsUser          *auser,
                       GDBusMethodInvocation *context,
                       const gchar           *session_type)
{
        User *user = (User*)auser;
        int uid;
        const gchar *action_id;

        if (!get_caller_uid (context, &uid)) {
                throw_error (context, ERROR_FAILED, "identifying caller failed");
                return FALSE;
        }

        if (accounts_user_get_uid (ACCOUNTS_USER (user)) == (uid_t) uid)
                action_id = "org.freedesktop.accounts.change-own-user-data";
        else
                action_id = "org.freedesktop.accounts.user-administration";

        daemon_local_check_auth (user->daemon,
                                 user,
                                 action_id,
                                 TRUE,
                                 user_change_session_type_authorized_cb,
                                 context,
                                 g_strdup (session_type),
                                 (GDestroyNotify) g_free);

        return TRUE;
}

static void
user_change_x_session_authorized_cb (Daemon                *daemon,
                                     User                  *user,
                                     GDBusMethodInvocation *context,
                                     gpointer               data)

{
        gchar *x_session = data;

        if (g_strcmp0 (accounts_user_get_xsession (ACCOUNTS_USER (user)), x_session) != 0) {
                accounts_user_set_xsession (ACCOUNTS_USER (user), x_session);

                save_extra_data (user);
        }

        accounts_user_complete_set_xsession (ACCOUNTS_USER (user), context);
}

static gboolean
user_set_x_session (AccountsUser          *auser,
                    GDBusMethodInvocation *context,
                    const gchar           *x_session)
{
        User *user = (User*)auser;
        int uid;
        const gchar *action_id;

        if (!get_caller_uid (context, &uid)) {
                throw_error (context, ERROR_FAILED, "identifying caller failed");
                return FALSE;
        }

        if (accounts_user_get_uid (ACCOUNTS_USER (user)) == (uid_t) uid)
                action_id = "org.freedesktop.accounts.change-own-user-data";
        else
                action_id = "org.freedesktop.accounts.user-administration";

        daemon_local_check_auth (user->daemon,
                                 user,
                                 action_id,
                                 TRUE,
                                 user_change_x_session_authorized_cb,
                                 context,
                                 g_strdup (x_session),
                                 (GDestroyNotify) g_free);

        return TRUE;
}

static void
user_get_password_expiration_policy_authorized_cb (Daemon                *daemon,
                                                   User                  *user,
                                                   GDBusMethodInvocation *context,
                                                   gpointer               data)

{
        if (!user->account_expiration_policy_known) {
                throw_error (context, ERROR_NOT_SUPPORTED, "account expiration policy unknown to accounts service");
                return;
        }

        accounts_user_complete_get_password_expiration_policy (ACCOUNTS_USER (user),
                                                               context,
                                                               user->expiration_time,
                                                               user->last_change_time,
                                                               user->min_days_between_changes,
                                                               user->max_days_between_changes,
                                                               user->days_to_warn,
                                                               user->days_after_expiration_until_lock);
}

static gboolean
user_get_password_expiration_policy (AccountsUser          *auser,
                                     GDBusMethodInvocation *context)
{
        User *user = (User*)auser;
        int uid;
        const gchar *action_id;

        if (!get_caller_uid (context, &uid)) {
                throw_error (context, ERROR_FAILED, "identifying caller failed");
                return FALSE;
        }

        if (accounts_user_get_uid (ACCOUNTS_USER (user)) == (uid_t) uid)
                action_id = "org.freedesktop.accounts.change-own-user-data";
        else
                action_id = "org.freedesktop.accounts.user-administration";

        daemon_local_check_auth (user->daemon,
                                 user,
                                 action_id,
                                 TRUE,
                                 user_get_password_expiration_policy_authorized_cb,
                                 context,
                                 NULL,
                                 NULL);

        return TRUE;
}


static void
user_change_location_authorized_cb (Daemon                *daemon,
                                    User                  *user,
                                    GDBusMethodInvocation *context,
                                    gpointer               data)

{
        gchar *location = data;

        if (g_strcmp0 (accounts_user_get_location (ACCOUNTS_USER (user)), location) != 0) {
                accounts_user_set_location (ACCOUNTS_USER (user), location);

                save_extra_data (user);
        }

        accounts_user_complete_set_location (ACCOUNTS_USER (user), context);
}

static gboolean
user_set_location (AccountsUser          *auser,
                   GDBusMethodInvocation *context,
                   const gchar           *location)
{
        User *user = (User*)auser;
        int uid;
        const gchar *action_id;

        if (!get_caller_uid (context, &uid)) {
                throw_error (context, ERROR_FAILED, "identifying caller failed");
                return FALSE;
        }

        if (accounts_user_get_uid (ACCOUNTS_USER (user)) == (uid_t) uid)
                action_id = "org.freedesktop.accounts.change-own-user-data";
        else
                action_id = "org.freedesktop.accounts.user-administration";

        daemon_local_check_auth (user->daemon,
                                 user,
                                 action_id,
                                 TRUE,
                                 user_change_location_authorized_cb,
                                 context,
                                 g_strdup (location),
                                 (GDestroyNotify)g_free);

        return TRUE;
}

static void
user_change_home_dir_authorized_cb (Daemon                *daemon,
                                    User                  *user,
                                    GDBusMethodInvocation *context,
                                    gpointer               data)

{
        gchar *home_dir = data;
        g_autoptr(GError) error = NULL;
        const gchar *argv[7];

        if (g_strcmp0 (accounts_user_get_home_directory (ACCOUNTS_USER (user)), home_dir) != 0) {
                sys_log (context,
                         "change home directory of user '%s' (%d) to '%s'",
                         accounts_user_get_user_name (ACCOUNTS_USER (user)),
                         accounts_user_get_uid (ACCOUNTS_USER (user)),
                         home_dir);

                argv[0] = "/usr/sbin/usermod";
                argv[1] = "-m";
                argv[2] = "-d";
                argv[3] = home_dir;
                argv[4] = "--";
                argv[5] = accounts_user_get_user_name (ACCOUNTS_USER (user));
                argv[6] = NULL;

                if (!spawn_with_login_uid (context, argv, &error)) {
                        throw_error (context, ERROR_FAILED, "running '%s' failed: %s", argv[0], error->message);
                        return;
                }

                accounts_user_set_home_directory (ACCOUNTS_USER (user), home_dir);

                user_reset_icon_file (user);
        }

        accounts_user_complete_set_home_directory (ACCOUNTS_USER (user), context);
}

static gboolean
user_set_home_directory (AccountsUser          *auser,
                         GDBusMethodInvocation *context,
                         const gchar           *home_dir)
{
        User *user = (User*)auser;
        daemon_local_check_auth (user->daemon,
                                 user,
                                 "org.freedesktop.accounts.user-administration",
                                 TRUE,
                                 user_change_home_dir_authorized_cb,
                                 context,
                                 g_strdup (home_dir),
                                 (GDestroyNotify)g_free);

        return TRUE;
}

static void
user_change_shell_authorized_cb (Daemon                *daemon,
                                 User                  *user,
                                 GDBusMethodInvocation *context,
                                 gpointer               data)

{
        gchar *shell = data;
        g_autoptr(GError) error = NULL;
        const gchar *argv[6];

        if (g_strcmp0 (accounts_user_get_shell (ACCOUNTS_USER (user)), shell) != 0) {
                sys_log (context,
                         "change shell of user '%s' (%d) to '%s'",
                         accounts_user_get_user_name (ACCOUNTS_USER (user)),
                         accounts_user_get_uid (ACCOUNTS_USER (user)),
                         shell);

                argv[0] = "/usr/sbin/usermod";
                argv[1] = "-s";
                argv[2] = shell;
                argv[3] = "--";
                argv[4] = accounts_user_get_user_name (ACCOUNTS_USER (user));
                argv[5] = NULL;

                if (!spawn_with_login_uid (context, argv, &error)) {
                        throw_error (context, ERROR_FAILED, "running '%s' failed: %s", argv[0], error->message);
                        return;
                }

                accounts_user_set_shell (ACCOUNTS_USER (user), shell);
        }

        accounts_user_complete_set_shell (ACCOUNTS_USER (user), context);
}

static gboolean
user_set_shell (AccountsUser          *auser,
                GDBusMethodInvocation *context,
                const gchar           *shell)
{
        User *user = (User*)auser;
        daemon_local_check_auth (user->daemon,
                                 user,
                                 "org.freedesktop.accounts.user-administration",
                                 TRUE,
                                 user_change_shell_authorized_cb,
                                 context,
                                 g_strdup (shell),
                                 (GDestroyNotify)g_free);

        return TRUE;
}

static void
become_user (gpointer data)
{
        struct passwd *pw = data;

        if (pw == NULL ||
            initgroups (pw->pw_name, pw->pw_gid) != 0 ||
            setgid (pw->pw_gid) != 0 ||
            setuid (pw->pw_uid) != 0) {
                exit (1);
        }
}

static void
user_change_icon_file_authorized_cb (Daemon                *daemon,
                                     User                  *user,
                                     GDBusMethodInvocation *context,
                                     gpointer               data)

{
        g_autofree gchar *filename = NULL;
        g_autoptr(GFile) file = NULL;
        g_autoptr(GFileInfo) info = NULL;
        guint32 mode;
        GFileType type;
        guint64 size;

        filename = g_strdup (data);

        if (filename == NULL ||
            *filename == '\0') {
                g_autofree gchar *dest_path = NULL;
                g_autoptr(GFile) dest = NULL;
                g_autoptr(GError) error = NULL;

                g_clear_pointer (&filename, g_free);

                dest_path = g_build_filename (ICONDIR, accounts_user_get_user_name (ACCOUNTS_USER (user)), NULL);
                dest = g_file_new_for_path (dest_path);

                if (!g_file_delete (dest, NULL, &error) &&
                    !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
                        throw_error (context, ERROR_FAILED, "failed to remove user icon, %s", error->message);
                        return;
                }
                goto icon_saved;
        }

        file = g_file_new_for_path (filename);
        g_clear_pointer (&filename, g_free);

        /* Canonicalize path so we can call g_str_has_prefix on it
         * below without concern for ../ path components moving outside
         * the prefix
         */
        filename = g_file_get_path (file);

        info = g_file_query_info (file, G_FILE_ATTRIBUTE_UNIX_MODE ","
                                        G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                                        G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                  0, NULL, NULL);
        mode = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_MODE);
        type = g_file_info_get_file_type (info);
        size = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_STANDARD_SIZE);

        if (type != G_FILE_TYPE_REGULAR) {
                g_debug ("not a regular file");
                throw_error (context, ERROR_FAILED, "file '%s' is not a regular file", filename);
                return;
        }

        if (size > 1048576) {
                g_debug ("file too large");
                /* 1MB ought to be enough for everybody */
                throw_error (context, ERROR_FAILED, "file '%s' is too large to be used as an icon", filename);
                return;
        }

        if ((mode & S_IROTH) == 0 ||
            (!g_str_has_prefix (filename, DATADIR) &&
             !g_str_has_prefix (filename, ICONDIR))) {
                g_autofree gchar *dest_path = NULL;
                g_autoptr(GFile) dest = NULL;
                const gchar *argv[3];
                gint std_out;
                g_autoptr(GError) error = NULL;
                g_autoptr(GInputStream) input = NULL;
                g_autoptr(GOutputStream) output = NULL;
                gint uid;
                gssize bytes;
                struct passwd *pw;

                if (!get_caller_uid (context, &uid)) {
                        throw_error (context, ERROR_FAILED, "failed to copy file, could not determine caller UID");
                        return;
                }

                dest_path = g_build_filename (ICONDIR, accounts_user_get_user_name (ACCOUNTS_USER (user)), NULL);
                dest = g_file_new_for_path (dest_path);

                output = G_OUTPUT_STREAM (g_file_replace (dest, NULL, FALSE, 0, NULL, &error));
                if (!output) {
                        throw_error (context, ERROR_FAILED, "creating file '%s' failed: %s", dest_path, error->message);
                        return;
                }

                argv[0] = "/bin/cat";
                argv[1] = filename;
                argv[2] = NULL;

                pw = getpwuid (uid);

                if (!g_spawn_async_with_pipes (NULL, (gchar**)argv, NULL, 0, become_user, pw, NULL, NULL, &std_out, NULL, &error)) {
                        throw_error (context, ERROR_FAILED, "reading file '%s' failed: %s", filename, error->message);
                        return;
                }

                input = g_unix_input_stream_new (std_out, FALSE);

                bytes = g_output_stream_splice (output, input, G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET, NULL, &error);
                if (bytes < 0 || (gsize)bytes != size) {
                        throw_error (context, ERROR_FAILED, "copying file '%s' to '%s' failed: %s", filename, dest_path, error ? error->message : "unknown reason");
                        g_file_delete (dest, NULL, NULL);
                        return;
                }

                g_free (filename);
                filename = g_steal_pointer (&dest_path);
        }

icon_saved:
        accounts_user_set_icon_file (ACCOUNTS_USER (user), filename);
        g_clear_pointer (&filename, g_free);

        save_extra_data (user);

        accounts_user_complete_set_icon_file (ACCOUNTS_USER (user), context);
}

static gboolean
user_set_icon_file (AccountsUser          *auser,
                    GDBusMethodInvocation *context,
                    const gchar           *filename)
{
        User *user = (User*)auser;
        int uid;
        const gchar *action_id;

        if (!get_caller_uid (context, &uid)) {
                throw_error (context, ERROR_FAILED, "identifying caller failed");
                return FALSE;
        }

        if (accounts_user_get_uid (ACCOUNTS_USER (user)) == (uid_t) uid)
                action_id = "org.freedesktop.accounts.change-own-user-data";
        else
                action_id = "org.freedesktop.accounts.user-administration";

        daemon_local_check_auth (user->daemon,
                                 user,
                                 action_id,
                                 TRUE,
                                 user_change_icon_file_authorized_cb,
                                 context,
                                 g_strdup (filename),
                                 (GDestroyNotify)g_free);

        return TRUE;
}

static void
user_change_locked_authorized_cb (Daemon                *daemon,
                                  User                  *user,
                                  GDBusMethodInvocation *context,
                                  gpointer               data)

{
        gboolean locked = GPOINTER_TO_INT (data);
        g_autoptr(GError) error = NULL;
        const gchar *argv[5];

        if (accounts_user_get_locked (ACCOUNTS_USER (user)) != locked) {
                sys_log (context,
                         "%s account of user '%s' (%d)",
                         locked ? "locking" : "unlocking",
                         accounts_user_get_user_name (ACCOUNTS_USER (user)),
                         accounts_user_get_uid (ACCOUNTS_USER (user)));
                argv[0] = "/usr/sbin/usermod";
                argv[1] = locked ? "-L" : "-U";
                argv[2] = "--";
                argv[3] = accounts_user_get_user_name (ACCOUNTS_USER (user));
                argv[4] = NULL;

                if (!spawn_with_login_uid (context, argv, &error)) {
                        throw_error (context, ERROR_FAILED, "running '%s' failed: %s", argv[0], error->message);
                        return;
                }

                accounts_user_set_locked (ACCOUNTS_USER (user), locked);

                if (accounts_user_get_automatic_login (ACCOUNTS_USER (user))) {
                    User *automatic_login_user;

                    automatic_login_user = daemon_local_get_automatic_login_user (daemon);
                    if (accounts_user_get_locked (ACCOUNTS_USER (user))) {
                            /* If automatic login is enabled for the user then
                             * disable it in the config file, but keep the state
                             * attached to the user unharmed so it can be restored
                             * later in the session
                             */
                            if (user == automatic_login_user) {
                                    daemon_local_set_automatic_login (daemon, user, FALSE, NULL);
                                    accounts_user_set_automatic_login (ACCOUNTS_USER (user), TRUE);
                            }
                    } else {
                            if (automatic_login_user == NULL) {
                                    accounts_user_set_automatic_login (ACCOUNTS_USER (user), FALSE);
                                    daemon_local_set_automatic_login (daemon, user, TRUE, NULL);
                            }
                    }
                }

                accounts_user_emit_changed (ACCOUNTS_USER (user));

                g_object_notify (G_OBJECT (user), "locked");
        }

        accounts_user_complete_set_locked (ACCOUNTS_USER (user), context);
}

static gboolean
user_set_locked (AccountsUser          *auser,
                 GDBusMethodInvocation *context,
                 gboolean               locked)
{
        User *user = (User*)auser;
        daemon_local_check_auth (user->daemon,
                                 user,
                                 "org.freedesktop.accounts.user-administration",
                                 TRUE,
                                 user_change_locked_authorized_cb,
                                 context,
                                 GINT_TO_POINTER (locked),
                                 NULL);

        return TRUE;
}

static void
user_change_account_type_authorized_cb (Daemon                *daemon,
                                        User                  *user,
                                        GDBusMethodInvocation *context,
                                        gpointer               data)

{
        AccountType account_type = GPOINTER_TO_INT (data);
        g_autoptr(GError) error = NULL;
        gid_t *groups;
        gint ngroups;
        g_autoptr(GString) str = NULL;
        g_auto(GStrv) extra_admin_groups = NULL;
        gid_t admin_gid;
        struct group *grp;
        gint i;
        const gchar *argv[6];

        if (((AccountType) accounts_user_get_account_type (ACCOUNTS_USER (user))) != account_type) {
                sys_log (context,
                         "change account type of user '%s' (%d) to %d",
                         accounts_user_get_user_name (ACCOUNTS_USER (user)),
                         accounts_user_get_uid (ACCOUNTS_USER (user)),
                         account_type);

                grp = getgrnam (ADMIN_GROUP);
                if (grp == NULL) {
                        throw_error (context, ERROR_FAILED, "failed to set account type: " ADMIN_GROUP " group not found");
                        return;
                }
                admin_gid = grp->gr_gid;

                ngroups = get_user_groups (accounts_user_get_user_name (ACCOUNTS_USER (user)), user->gid, &groups);

                str = g_string_new ("");
                for (i = 0; i < ngroups; i++) {
                        if (groups[i] == admin_gid)
                                continue;
                        g_string_append_printf (str, "%d,", groups[i]);
                }
                switch (account_type) {
                case ACCOUNT_TYPE_ADMINISTRATOR:
                        extra_admin_groups = g_strsplit (EXTRA_ADMIN_GROUPS, ",", 0);

                        for (i = 0; extra_admin_groups[i] != NULL; i++) {
                                struct group *extra_group;
                                extra_group = getgrnam (extra_admin_groups[i]);
                                if (extra_group == NULL || extra_group->gr_gid == admin_gid)
                                        continue;

                                g_string_append_printf (str, "%d,", extra_group->gr_gid);
                        }

                        g_string_append_printf (str, "%d", admin_gid);
                        break;
                case ACCOUNT_TYPE_STANDARD:
                default:
                        /* remove excess comma */
                        g_string_truncate (str, str->len - 1);
                        break;
                }

                g_free (groups);

                argv[0] = "/usr/sbin/usermod";
                argv[1] = "-G";
                argv[2] = str->str;
                argv[3] = "--";
                argv[4] = accounts_user_get_user_name (ACCOUNTS_USER (user));
                argv[5] = NULL;

                if (!spawn_with_login_uid (context, argv, &error)) {
                        throw_error (context, ERROR_FAILED, "running '%s' failed: %s", argv[0], error->message);
                        return;
                }

                accounts_user_set_account_type (ACCOUNTS_USER (user), account_type);

                accounts_user_emit_changed (ACCOUNTS_USER (user));

                g_object_notify (G_OBJECT (user), "account-type");
        }

        accounts_user_complete_set_account_type (ACCOUNTS_USER (user), context);
}

static gboolean
user_set_account_type (AccountsUser          *auser,
                       GDBusMethodInvocation *context,
                       gint                   account_type)
{
        User *user = (User*)auser;
        if (account_type < 0 || account_type > ACCOUNT_TYPE_LAST) {
                throw_error (context, ERROR_FAILED, "unknown account type: %d", account_type);
                return FALSE;
        }

        daemon_local_check_auth (user->daemon,
                                 user,
                                 "org.freedesktop.accounts.user-administration",
                                 TRUE,
                                 user_change_account_type_authorized_cb,
                                 context,
                                 GINT_TO_POINTER (account_type),
                                 NULL);

        return TRUE;
}

static void
user_change_password_mode_authorized_cb (Daemon                *daemon,
                                         User                  *user,
                                         GDBusMethodInvocation *context,
                                         gpointer               data)

{
        PasswordMode mode = GPOINTER_TO_INT (data);
        g_autoptr(GError) error = NULL;
        const gchar *argv[6];

        if (((PasswordMode) accounts_user_get_password_mode (ACCOUNTS_USER (user))) != mode) {
                sys_log (context,
                         "change password mode of user '%s' (%d) to %d",
                         accounts_user_get_user_name (ACCOUNTS_USER (user)),
                         accounts_user_get_uid (ACCOUNTS_USER (user)),
                         mode);

                g_object_freeze_notify (G_OBJECT (user));

                if (mode == PASSWORD_MODE_SET_AT_LOGIN ||
                    mode == PASSWORD_MODE_NONE) {

                        argv[0] = "/usr/bin/passwd";
                        argv[1] = "-d";
                        argv[2] = "--";
                        argv[3] = accounts_user_get_user_name (ACCOUNTS_USER (user));
                        argv[4] = NULL;

                        if (!spawn_with_login_uid (context, argv, &error)) {
                                throw_error (context, ERROR_FAILED, "running '%s' failed: %s", argv[0], error->message);
                                return;
                        }

                        if (mode == PASSWORD_MODE_SET_AT_LOGIN) {
                                argv[0] = "/usr/bin/chage";
                                argv[1] = "-d";
                                argv[2] = "0";
                                argv[3] = "--";
                                argv[4] = accounts_user_get_user_name (ACCOUNTS_USER (user));
                                argv[5] = NULL;

                                if (!spawn_with_login_uid (context, argv, &error)) {
                                        throw_error (context, ERROR_FAILED, "running '%s' failed: %s", argv[0], error->message);
                                        return;
                                }
                        }

                        accounts_user_set_password_hint (ACCOUNTS_USER (user), NULL);

                        /* removing the password has the side-effect of
                         * unlocking the account
                         */
                        accounts_user_set_locked (ACCOUNTS_USER (user), FALSE);
                }
                else if (accounts_user_get_locked (ACCOUNTS_USER (user))) {
                        argv[0] = "/usr/sbin/usermod";
                        argv[1] = "-U";
                        argv[2] = "--";
                        argv[3] = accounts_user_get_user_name (ACCOUNTS_USER (user));
                        argv[4] = NULL;

                        if (!spawn_with_login_uid (context, argv, &error)) {
                                throw_error (context, ERROR_FAILED, "running '%s' failed: %s", argv[0], error->message);
                                return;
                        }

                        accounts_user_set_locked (ACCOUNTS_USER (user), FALSE);
                }

                accounts_user_set_password_mode (ACCOUNTS_USER (user), mode);

                save_extra_data (user);

                g_object_thaw_notify (G_OBJECT (user));
        }

        accounts_user_complete_set_password_mode (ACCOUNTS_USER (user), context);
}

static gboolean
user_set_password_mode (AccountsUser          *auser,
                        GDBusMethodInvocation *context,
                        gint                   mode)
{
        User *user = (User*)auser;
        const gchar *action_id;
        gint uid;

        if (mode < 0 || mode > PASSWORD_MODE_LAST) {
                throw_error (context, ERROR_FAILED, "unknown password mode: %d", mode);
                return FALSE;
        }

        if (!get_caller_uid (context, &uid)) {
                throw_error (context, ERROR_FAILED, "identifying caller failed");
                return FALSE;
        }

        if (accounts_user_get_uid (ACCOUNTS_USER (user)) == (uid_t) uid)
                action_id = "org.freedesktop.accounts.change-own-password";
        else
                action_id = "org.freedesktop.accounts.user-administration";

        daemon_local_check_auth (user->daemon,
                                 user,
                                 action_id,
                                 TRUE,
                                 user_change_password_mode_authorized_cb,
                                 context,
                                 GINT_TO_POINTER (mode),
                                 NULL);

        return TRUE;
}

static void
user_change_password_authorized_cb (Daemon                *daemon,
                                    User                  *user,
                                    GDBusMethodInvocation *context,
                                    gpointer               data)

{
        gchar **strings = data;
        g_autoptr(GError) error = NULL;
        const gchar *argv[6];

        sys_log (context,
                 "set password and hint of user '%s' (%d)",
                 accounts_user_get_user_name (ACCOUNTS_USER (user)),
                 accounts_user_get_uid (ACCOUNTS_USER (user)));

        g_object_freeze_notify (G_OBJECT (user));

        argv[0] = "/usr/sbin/usermod";
        argv[1] = "-p";
        argv[2] = strings[0];
        argv[3] = "--";
        argv[4] = accounts_user_get_user_name (ACCOUNTS_USER (user));
        argv[5] = NULL;

        if (!spawn_with_login_uid (context, argv, &error)) {
                throw_error (context, ERROR_FAILED, "running '%s' failed: %s", argv[0], error->message);
                return;
        }

        accounts_user_set_password_mode (ACCOUNTS_USER (user), PASSWORD_MODE_REGULAR);
        accounts_user_set_locked (ACCOUNTS_USER (user), FALSE);
        accounts_user_set_password_hint (ACCOUNTS_USER (user), strings[1]);

        save_extra_data (user);

        g_object_thaw_notify (G_OBJECT (user));

        accounts_user_complete_set_password (ACCOUNTS_USER (user), context);
}

static void
free_passwords (gchar **strings)
{
        memset (strings[0], 0, strlen (strings[0]));
        g_strfreev (strings);
}

static gboolean
user_set_password (AccountsUser          *auser,
                   GDBusMethodInvocation *context,
                   const gchar           *password,
                   const gchar           *hint)
{
        User *user = (User*)auser;
        gchar **data;
        const gchar *action_id;
        gint uid;

        if (!get_caller_uid (context, &uid)) {
                throw_error (context, ERROR_FAILED, "identifying caller failed");
                return FALSE;
        }

        data = g_new (gchar *, 3);
        data[0] = g_strdup (password);
        data[1] = g_strdup (hint);
        data[2] = NULL;

        if (accounts_user_get_uid (ACCOUNTS_USER (user)) == (uid_t) uid)
                action_id = "org.freedesktop.accounts.change-own-password";
        else
                action_id = "org.freedesktop.accounts.user-administration";

        daemon_local_check_auth (user->daemon,
                                 user,
                                 action_id,
                                 TRUE,
                                 user_change_password_authorized_cb,
                                 context,
                                 data,
                                 (GDestroyNotify)free_passwords);

        memset ((char*)password, 0, strlen (password));

        return TRUE;
}

static void
user_change_password_hint_authorized_cb (Daemon                *daemon,
                                         User                  *user,
                                         GDBusMethodInvocation *context,
                                         gpointer               data)
{
        gchar *hint = data;

        sys_log (context,
                 "set password hint of user '%s' (%d)'",
                 accounts_user_get_user_name (ACCOUNTS_USER (user)),
                 accounts_user_get_uid (ACCOUNTS_USER (user)));

        if (g_strcmp0 (accounts_user_get_password_hint (ACCOUNTS_USER (user)), hint) != 0) {
                accounts_user_set_password_hint (ACCOUNTS_USER (user), hint);

                save_extra_data (user);
        }

        accounts_user_complete_set_password_hint (ACCOUNTS_USER (user), context);
}

static gboolean
user_set_password_hint (AccountsUser          *auser,
                        GDBusMethodInvocation *context,
                        const gchar           *hint)
{
        User *user = (User*)auser;
        int uid;
        const gchar *action_id;

        if (!get_caller_uid (context, &uid)) {
                throw_error (context, ERROR_FAILED, "identifying caller failed");
                return FALSE;
        }

        if (accounts_user_get_uid (ACCOUNTS_USER (user)) == (uid_t) uid)
                action_id = "org.freedesktop.accounts.change-own-user-data";
        else
                action_id = "org.freedesktop.accounts.user-administration";

        daemon_local_check_auth (user->daemon,
                                 user,
                                 action_id,
                                 TRUE,
                                 user_change_password_hint_authorized_cb,
                                 context,
                                 g_strdup (hint),
                                 (GDestroyNotify)g_free);

        return TRUE;
}

static void
user_change_automatic_login_authorized_cb (Daemon                *daemon,
                                           User                  *user,
                                           GDBusMethodInvocation *context,
                                           gpointer               data)
{
        gboolean enabled = GPOINTER_TO_INT (data);
        g_autoptr(GError) error = NULL;

        sys_log (context,
                 "%s automatic login for user '%s' (%d)",
                 enabled ? "enable" : "disable",
                 accounts_user_get_user_name (ACCOUNTS_USER (user)),
                 accounts_user_get_uid (ACCOUNTS_USER (user)));

        if (accounts_user_get_locked (ACCOUNTS_USER (user))) {
                throw_error (context, ERROR_FAILED, "failed to change automatic login: user is locked");
                return;
        }

        if (!daemon_local_set_automatic_login (daemon, user, enabled, &error)) {
                throw_error (context, ERROR_FAILED, "failed to change automatic login: %s", error->message);
                return;
        }

        accounts_user_complete_set_automatic_login (ACCOUNTS_USER (user), context);
}

static gboolean
user_set_automatic_login (AccountsUser          *auser,
                          GDBusMethodInvocation *context,
                          gboolean               enabled)
{
        User *user = (User*)auser;
        daemon_local_check_auth (user->daemon,
                                 user,
                                 "org.freedesktop.accounts.user-administration",
                                 TRUE,
                                 user_change_automatic_login_authorized_cb,
                                 context,
                                 GINT_TO_POINTER (enabled),
                                 NULL);

        return TRUE;
}

static void
user_finalize (GObject *object)
{
        User *user;

        user = USER (object);

        if (user->changed_timeout_id != 0)
            g_source_remove (user->changed_timeout_id);

        g_clear_pointer (&user->keyfile, g_key_file_unref);

        g_free (user->default_icon_file);

	g_clear_pointer (&user->login_history, g_variant_unref);

        if (G_OBJECT_CLASS (user_parent_class)->finalize)
                (*G_OBJECT_CLASS (user_parent_class)->finalize) (object);
}

static void
user_class_init (UserClass *class)
{
        GObjectClass *gobject_class;

        gobject_class = G_OBJECT_CLASS (class);

        gobject_class->finalize = user_finalize;
}

static void
user_accounts_user_iface_init (AccountsUserIface *iface)
{
        iface->handle_set_account_type = user_set_account_type;
        iface->handle_set_automatic_login = user_set_automatic_login;
        iface->handle_set_email = user_set_email;
        iface->handle_set_home_directory = user_set_home_directory;
        iface->handle_set_icon_file = user_set_icon_file;
        iface->handle_set_language = user_set_language;
        iface->handle_set_location = user_set_location;
        iface->handle_set_locked = user_set_locked;
        iface->handle_set_password = user_set_password;
        iface->handle_set_password_mode = user_set_password_mode;
        iface->handle_set_password_hint = user_set_password_hint;
        iface->handle_set_real_name = user_set_real_name;
        iface->handle_set_shell = user_set_shell;
        iface->handle_set_user_name = user_set_user_name;
        iface->handle_set_xsession = user_set_x_session;
        iface->handle_set_session = user_set_session;
        iface->handle_set_session_type = user_set_session_type;
        iface->handle_get_password_expiration_policy = user_get_password_expiration_policy;
}

static void
user_init (User *user)
{
        user->system_bus_connection = NULL;
        user->default_icon_file = NULL;
        user->login_history = NULL;
        user->keyfile = g_key_file_new ();
}
