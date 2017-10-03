/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007-2008 William Jon McCann <mccann@jhu.edu>
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
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef HAVE_PATHS_H
#include <paths.h>
#endif /* HAVE_PATHS_H */

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>
#include <glib-object.h>
#include <gio/gio.h>
#include <gio/gunixinputstream.h>

#ifdef WITH_SYSTEMD
#include <systemd/sd-login.h>

/* check if logind is running */
#define LOGIND_RUNNING() (access("/run/systemd/seats/", F_OK) >= 0)
#endif

#include "act-user-manager.h"
#include "act-user-private.h"
#include "accounts-generated.h"
#include "ck-manager-generated.h"
#include "ck-seat-generated.h"
#include "ck-session-generated.h"

/**
 * SECTION:act-user-manager
 * @title: ActUserManager
 * @short_description: manages ActUser objects
 *
 * ActUserManager is a manager object that gives access to user
 * creation, deletion, enumeration, etc.
 *
 * There is typically a singleton ActUserManager object, which
 * can be obtained by act_user_manager_get_default().
 */

/**
 * ActUserManager:
 *
 * A user manager object.
 */

/**
 * ACT_USER_MANAGER_ERROR:
 *
 * The GError domain for #ActUserManagerError errors
 */

/**
 * ActUserManagerError:
 * @ACT_USER_MANAGER_ERROR_FAILED: Generic failure
 * @ACT_USER_MANAGER_ERROR_USER_EXISTS: The user already exists
 * @ACT_USER_MANAGER_ERROR_USER_DOES_NOT_EXIST: The user does not exist
 * @ACT_USER_MANAGER_ERROR_PERMISSION_DENIED: Permission denied
 * @ACT_USER_MANAGER_ERROR_NOT_SUPPORTED: Operation not supported
 *
 * Various error codes returned by the accounts service.
 */

#define ACT_USER_MANAGER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), ACT_TYPE_USER_MANAGER, ActUserManagerPrivate))

#define CK_NAME      "org.freedesktop.ConsoleKit"

#define CK_MANAGER_PATH      "/org/freedesktop/ConsoleKit/Manager"
#define CK_MANAGER_INTERFACE "org.freedesktop.ConsoleKit.Manager"
#define CK_SEAT_INTERFACE    "org.freedesktop.ConsoleKit.Seat"
#define CK_SESSION_INTERFACE "org.freedesktop.ConsoleKit.Session"

#define ACCOUNTS_NAME      "org.freedesktop.Accounts"
#define ACCOUNTS_PATH      "/org/freedesktop/Accounts"
#define ACCOUNTS_INTERFACE "org.freedesktop.Accounts"

typedef enum {
        ACT_USER_MANAGER_SEAT_STATE_UNLOADED = 0,
        ACT_USER_MANAGER_SEAT_STATE_GET_SESSION_ID,
        ACT_USER_MANAGER_SEAT_STATE_GET_SESSION_PROXY,
        ACT_USER_MANAGER_SEAT_STATE_GET_ID,
        ACT_USER_MANAGER_SEAT_STATE_GET_SEAT_PROXY,
        ACT_USER_MANAGER_SEAT_STATE_LOADED,
} ActUserManagerSeatState;

typedef struct
{
        ActUserManagerSeatState      state;
        char                        *id;
        char                        *session_id;
        ConsoleKitSeat              *seat_proxy;
        ConsoleKitSession           *session_proxy;
        guint                        load_idle_id;
#ifdef WITH_SYSTEMD
        sd_login_monitor            *session_monitor;
        GInputStream                *session_monitor_stream;
        guint                        session_monitor_source_id;
#endif
} ActUserManagerSeat;

typedef enum {
        ACT_USER_MANAGER_NEW_SESSION_STATE_UNLOADED = 0,
        ACT_USER_MANAGER_NEW_SESSION_STATE_GET_PROXY,
        ACT_USER_MANAGER_NEW_SESSION_STATE_GET_UID,
        ACT_USER_MANAGER_NEW_SESSION_STATE_GET_X11_DISPLAY,
        ACT_USER_MANAGER_NEW_SESSION_STATE_MAYBE_ADD,
        ACT_USER_MANAGER_NEW_SESSION_STATE_LOADED,
} ActUserManagerNewSessionState;

typedef struct
{
        ActUserManager                  *manager;
        ActUserManagerNewSessionState    state;
        char                            *id;
        ConsoleKitSession               *proxy;
        GCancellable                    *cancellable;
        uid_t                            uid;
        char                            *x11_display;
        gsize                            pending_calls;
} ActUserManagerNewSession;

typedef enum {
        ACT_USER_MANAGER_GET_USER_STATE_UNFETCHED = 0,
        ACT_USER_MANAGER_GET_USER_STATE_WAIT_FOR_LOADED,
        ACT_USER_MANAGER_GET_USER_STATE_ASK_ACCOUNTS_SERVICE,
        ACT_USER_MANAGER_GET_USER_STATE_FETCHED
} ActUserManagerGetUserState;

typedef enum {
        ACT_USER_MANAGER_FETCH_USER_FROM_USERNAME_REQUEST,
        ACT_USER_MANAGER_FETCH_USER_FROM_ID_REQUEST,
} ActUserManagerFetchUserRequestType;

typedef struct
{
        ActUserManager             *manager;
        ActUserManagerGetUserState  state;
        ActUser                    *user;
        ActUserManagerFetchUserRequestType type;
        union {
                char               *username;
                uid_t               uid;
        };
        char                       *object_path;
        char                       *description;
} ActUserManagerFetchUserRequest;

struct ActUserManagerPrivate
{
        GHashTable            *normal_users_by_name;
        GHashTable            *system_users_by_name;
        GHashTable            *users_by_object_path;
        GHashTable            *sessions;
        GDBusConnection       *connection;
        AccountsAccounts      *accounts_proxy;
        ConsoleKitManager     *ck_manager_proxy;

        ActUserManagerSeat     seat;

        GSList                *new_sessions;
        GSList                *new_users;
        GSList                *new_users_inhibiting_load;
        GSList                *fetch_user_requests;

        GSList                *exclude_usernames;
        GSList                *include_usernames;

        guint                  load_id;

        gboolean               is_loaded;
        gboolean               has_multiple_users;
        gboolean               getting_sessions;
        gboolean               listing_cached_users;
};

enum {
        PROP_0,
        PROP_INCLUDE_USERNAMES_LIST,
        PROP_EXCLUDE_USERNAMES_LIST,
        PROP_IS_LOADED,
        PROP_HAS_MULTIPLE_USERS
};

enum {
        USER_ADDED,
        USER_REMOVED,
        USER_IS_LOGGED_IN_CHANGED,
        USER_CHANGED,
        LAST_SIGNAL
};

static guint signals [LAST_SIGNAL] = { 0, };

static void     act_user_manager_class_init (ActUserManagerClass *klass);
static void     act_user_manager_init       (ActUserManager      *user_manager);
static void     act_user_manager_finalize   (GObject             *object);

static gboolean ensure_accounts_proxy       (ActUserManager *manager);
static gboolean load_seat_incrementally     (ActUserManager *manager);
static void     unload_seat                 (ActUserManager *manager);
static void     load_users                  (ActUserManager *manager);
static void     act_user_manager_queue_load (ActUserManager *manager);
static void     queue_load_seat_and_users   (ActUserManager *manager);

static void     load_new_session_incrementally (ActUserManagerNewSession *new_session);
static void     set_is_loaded (ActUserManager *manager, gboolean is_loaded);

static void     on_new_user_loaded (ActUser        *user,
                                    GParamSpec     *pspec,
                                    ActUserManager *manager);
static void     give_up (ActUserManager                 *manager,
                         ActUserManagerFetchUserRequest *request);
static void     fetch_user_incrementally       (ActUserManagerFetchUserRequest *request);

static void     maybe_set_is_loaded            (ActUserManager *manager);
static void     update_user                    (ActUserManager *manager,
                                                ActUser        *user);
static gpointer user_manager_object = NULL;

G_DEFINE_TYPE (ActUserManager, act_user_manager, G_TYPE_OBJECT)

static const GDBusErrorEntry error_entries[] = {
        { ACT_USER_MANAGER_ERROR_FAILED,              "org.freedesktop.Accounts.Error.Failed" },
        { ACT_USER_MANAGER_ERROR_USER_EXISTS,         "org.freedesktop.Accounts.Error.UserExists" },
        { ACT_USER_MANAGER_ERROR_USER_DOES_NOT_EXIST, "org.freedesktop.Accounts.Error.UserDoesNotExist" },
        { ACT_USER_MANAGER_ERROR_PERMISSION_DENIED,   "org.freedesktop.Accounts.Error.PermissionDenied" },
        { ACT_USER_MANAGER_ERROR_NOT_SUPPORTED,       "org.freedesktop.Accounts.Error.NotSupported" }
};

GQuark
act_user_manager_error_quark (void)
{
        static volatile gsize ret = 0;
        if (ret == 0) {
                g_dbus_error_register_error_domain ("act_user_manager_error",
                                                    &ret,
                                                    error_entries,
                                                    G_N_ELEMENTS (error_entries));
        }

        return (GQuark) ret;
}

static gboolean
activate_console_kit_session_id (ActUserManager *manager,
                                 const char     *seat_id,
                                 const char     *session_id)
{
        ConsoleKitSeat *proxy;
        GError         *error = NULL;
        gboolean        res = FALSE;

        proxy = console_kit_seat_proxy_new_sync (manager->priv->connection,
                                                 G_DBUS_PROXY_FLAGS_NONE,
                                                 CK_NAME,
                                                 seat_id,
                                                 NULL,
                                                 &error);
        if (proxy)
                res = console_kit_seat_call_activate_session_sync (proxy,
                                                                   session_id,
                                                                   NULL,
                                                                   &error);

        if (!res) {
                g_warning ("Unable to activate session: %s", error->message);
                g_error_free (error);
                return FALSE;
        }

        return TRUE;
}

#ifdef WITH_SYSTEMD
static gboolean
activate_systemd_session_id (ActUserManager *manager,
                             const char     *seat_id,
                             const char     *session_id)
{
        GDBusConnection *connection;
        GVariant *reply;
        GError *error;

        error = NULL;
        connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);


        if (connection == NULL) {
                goto failed;
        }

        reply = g_dbus_connection_call_sync (connection,
                                             "org.freedesktop.login1",
                                             "/org/freedesktop/login1",
                                             "org.freedesktop.login1.Manager",
                                             "ActivateSessionOnSeat",
                                             g_variant_new ("(ss)",
                                                            seat_id,
                                                            session_id),
                                             NULL,
                                             G_DBUS_CALL_FLAGS_NONE,
                                             -1,
                                             NULL,
                                             &error);
        g_object_unref (connection);

        if (reply == NULL) {
                goto failed;
        }

        g_object_unref (reply);

        return TRUE;

failed:
        g_warning ("Unable to activate session: %s", error->message);
        g_error_free (error);
        return FALSE;
}
#endif

static gboolean
_ck_session_is_login_window (ActUserManager *manager,
                             const char     *session_id)
{
        ConsoleKitSession *proxy;
        GError            *error = NULL;
        char              *session_type;
        gboolean           res = FALSE;
        gboolean           ret;

        proxy = console_kit_session_proxy_new_sync (manager->priv->connection,
                                                    G_DBUS_PROXY_FLAGS_NONE,
                                                    CK_NAME,
                                                    session_id,
                                                    NULL,
                                                    &error);
        if (proxy)
                res = console_kit_session_call_get_session_type_sync (proxy, &session_type, NULL, &error);

        if (!res) {
                if (error != NULL) {
                        g_debug ("ActUserManager: Failed to identify the session type: %s", error->message);
                        g_error_free (error);
                } else {
                        g_debug ("ActUserManager: Failed to identify the session type");
                }
                return FALSE;
        }
        if (proxy)
                g_object_unref (proxy);

        ret = strcmp (session_type, "LoginWindow") == 0;
        g_free (session_type);

        return ret;
}

#ifdef WITH_SYSTEMD
static gboolean
_systemd_session_is_login_window (ActUserManager *manager,
                                  const char     *session_id)
{
        int   res;
        int   ret;
        char *session_class;

        ret = FALSE;
        res = sd_session_get_class (session_id, &session_class);

        if (res < 0) {
            g_debug ("failed to determine class of session %s: %s",
                     session_id,
                     strerror (-res));
            goto out;
        }

        if (g_strcmp0 (session_class, "greeter") == 0) {
            ret = TRUE;
        }

        free (session_class);

out:
        return ret;
}
#endif

static gboolean
session_is_login_window (ActUserManager *manager,
                         const char     *session_id)
{
#ifdef WITH_SYSTEMD
        if (LOGIND_RUNNING()) {
                return _systemd_session_is_login_window (manager, session_id);
        }
#endif

        return _ck_session_is_login_window (manager, session_id);
}

static gboolean
_ck_session_is_on_our_seat (ActUserManager *manager,
                            const char     *session_id)
{
        /* With ConsoleKit, we only ever see sessions on our seat. */
        return TRUE;
}

#ifdef WITH_SYSTEMD
static gboolean
_systemd_session_is_on_our_seat (ActUserManager *manager,
                                 const char     *session_id)
{
        int   res;
        int   ret;
        char *session_seat;

        ret = FALSE;
        res = sd_session_get_seat (session_id, &session_seat);
        if (res == -ENOENT) {
                goto out;
        } else if (res < 0) {
                g_debug ("failed to determine seat of session %s: %s",
                         session_id,
                         strerror (-res));
                goto out;
        }

        if (g_strcmp0 (manager->priv->seat.id, session_seat) == 0) {
                ret = TRUE;
        }

        free (session_seat);

out:
        return ret;
}
#endif

static gboolean
session_is_on_our_seat (ActUserManager *manager,
                        const char     *session_id)
{
#ifdef WITH_SYSTEMD
        if (LOGIND_RUNNING()) {
                return _systemd_session_is_on_our_seat (manager, session_id);
        }
#endif

        return _ck_session_is_on_our_seat (manager, session_id);
}

/**
 * act_user_manager_goto_login_session:
 * @manager: the user manager
 *
 * Switch the display to the login manager.
 *
 * Returns: whether successful or not
 */
gboolean
act_user_manager_goto_login_session (ActUserManager *manager)
{
        gboolean res;
        GError  *error;

        g_return_val_if_fail (ACT_IS_USER_MANAGER (manager), FALSE);
        g_return_val_if_fail (manager->priv->is_loaded, FALSE);

        res = g_spawn_command_line_async ("gdmflexiserver", &error);
        if (! res) {
                if (error != NULL) {
                        g_warning ("Unable to start new login: %s", error->message);
                        g_error_free (error);
                } else {
                        g_warning ("Unable to start new login");
                }
        }

        return res;

}

#ifdef WITH_SYSTEMD
gboolean
_can_activate_systemd_sessions (ActUserManager *manager)
{
        int res;

        res = sd_seat_can_multi_session (manager->priv->seat.id);
        if (res < 0) {
                g_warning ("unable to determine if seat can activate sessions: %s",
                           strerror (-res));
                return FALSE;
        }

        return res > 0;
}
#endif

gboolean
_can_activate_console_kit_sessions (ActUserManager *manager)
{
        GError   *error = NULL;
        gboolean  can_activate_sessions = FALSE;

        if (!console_kit_seat_call_can_activate_sessions_sync (manager->priv->seat.seat_proxy, &can_activate_sessions, NULL, &error)) {
                if (error != NULL) {
                        g_warning ("unable to determine if seat can activate sessions: %s",
                                   error->message);
                        g_error_free (error);
                } else {
                        g_warning ("unable to determine if seat can activate sessions");
                }
                return FALSE;
        }

        return can_activate_sessions;
}

/**
 * act_user_manager_can_switch:
 * @manager: the user manager
 *
 * Check whether the user can switch to another session.
 *
 * Returns: whether we can switch to another session
 */
gboolean
act_user_manager_can_switch (ActUserManager *manager)
{
        if (!manager->priv->is_loaded) {
                g_debug ("ActUserManager: Unable to switch sessions until fully loaded");
                return FALSE;
        }

        if (manager->priv->seat.id == NULL || manager->priv->seat.id[0] == '\0') {
                g_debug ("ActUserManager: display seat ID is not set; can't switch sessions");
                return FALSE;
        }

        g_debug ("ActUserManager: checking if seat can activate sessions");


#ifdef WITH_SYSTEMD
        if (LOGIND_RUNNING()) {
                return _can_activate_systemd_sessions (manager);
        }
#endif

        return _can_activate_console_kit_sessions (manager);
}

/**
 * act_user_manager_activate_user_session:
 * @manager: the user manager
 * @user: the user to activate
 *
 * Activate the session for a given user.
 *
 * Returns: whether successfully activated
 */
gboolean
act_user_manager_activate_user_session (ActUserManager *manager,
                                        ActUser        *user)
{
        gboolean ret;
        const char *ssid;
        gboolean res;

        gboolean can_activate_sessions;
        g_return_val_if_fail (ACT_IS_USER_MANAGER (manager), FALSE);
        g_return_val_if_fail (ACT_IS_USER (user), FALSE);
        g_return_val_if_fail (manager->priv->is_loaded, FALSE);

        ret = FALSE;

        can_activate_sessions = act_user_manager_can_switch (manager);

        if (! can_activate_sessions) {
                g_debug ("ActUserManager: seat is unable to activate sessions");
                goto out;
        }

        ssid = act_user_get_primary_session_id (user);
        if (ssid == NULL) {
                goto out;
        }

#ifdef WITH_SYSTEMD
        if (LOGIND_RUNNING()) {
                return activate_systemd_session_id (manager, manager->priv->seat.id, ssid);
        }
#endif

        res = activate_console_kit_session_id (manager, manager->priv->seat.id, ssid);
        if (! res) {
                g_debug ("ActUserManager: unable to activate session: %s", ssid);
                goto out;
        }

        ret = TRUE;
 out:
        return ret;
}

static const char *
describe_user (ActUser *user)
{
        ActUserManagerFetchUserRequest *request;

        if (act_user_is_loaded (user)) {
                static char *description = NULL;
                g_clear_pointer (&description, (GDestroyNotify) g_free);

                description = g_strdup_printf ("user %s", act_user_get_user_name (user));
                return description;
        }

        request = g_object_get_data (G_OBJECT (user), "fetch-user-request");

        if (request != NULL) {
                return request->description;
        }

        return "user";
}

static void
on_user_sessions_changed (ActUser        *user,
                          ActUserManager *manager)
{
        guint nsessions;

        if (! manager->priv->is_loaded) {
                return;
        }

        nsessions = act_user_get_num_sessions (user);

        g_debug ("ActUserManager: sessions changed (%s) num=%d",
                 describe_user (user),
                 nsessions);

        /* only signal on zero and one */
        if (nsessions > 1) {
                return;
        }

        g_signal_emit (manager, signals [USER_IS_LOGGED_IN_CHANGED], 0, user);
}

static void
on_user_changed (ActUser        *user,
                 ActUserManager *manager)
{
        if (manager->priv->is_loaded) {
                g_debug ("ActUserManager: sending user-changed signal for %s",
                         describe_user (user));

                g_signal_emit (manager, signals[USER_CHANGED], 0, user);

                g_debug ("ActUserManager: sent user-changed signal for %s",
                         describe_user (user));

                update_user (manager, user);
        }
}

static void
queue_load_seat_incrementally (ActUserManager *manager)
{
        if (manager->priv->seat.load_idle_id == 0) {
            manager->priv->seat.load_idle_id = g_idle_add ((GSourceFunc) load_seat_incrementally, manager);
        }
}

static void
on_get_seat_id_finished (GObject        *object,
                         GAsyncResult   *result,
                         gpointer        data)
{
        ConsoleKitSession *proxy = CONSOLE_KIT_SESSION (object);
        ActUserManager    *manager = data;
        GError            *error = NULL;
        char              *seat_id;

        if (!console_kit_session_call_get_seat_id_finish (proxy, &seat_id, result, &error)) {
                if (error != NULL) {
                        g_debug ("Failed to identify the seat of the "
                                 "current session: %s",
                                 error->message);
                        g_error_free (error);
                } else {
                        g_debug ("Failed to identify the seat of the "
                                 "current session");
                }

                g_debug ("ActUserManager: GetSeatId call failed, so unloading seat");
                unload_seat (manager);

                goto out;
        }

        g_debug ("ActUserManager: Found current seat: %s", seat_id);

        manager->priv->seat.id = seat_id;
        manager->priv->seat.state++;

        load_seat_incrementally (manager);

 out:
        g_debug ("ActUserManager: unrefing manager owned by GetSeatId request");
        g_object_unref (manager);
}

#ifdef WITH_SYSTEMD
static void
_get_systemd_seat_id (ActUserManager *manager)
{
        int   res;
        char *seat_id;

        res = sd_session_get_seat (NULL, &seat_id);

        if (res == -ENOENT) {
                seat_id = NULL;
        } else if (res < 0) {
                g_warning ("Could not get current seat: %s",
                           strerror (-res));
                unload_seat (manager);
                return;
        }

        manager->priv->seat.id = g_strdup (seat_id);
        free (seat_id);

        manager->priv->seat.state++;

        queue_load_seat_incrementally (manager);
}
#endif

static void
get_seat_id_for_current_session (ActUserManager *manager)
{
#ifdef WITH_SYSTEMD
        if (LOGIND_RUNNING()) {
                _get_systemd_seat_id (manager);
                return;
        }
#endif
        console_kit_session_call_get_seat_id (manager->priv->seat.session_proxy,
                                              NULL,
                                              on_get_seat_id_finished,
                                              g_object_ref (manager));
}

static gint
match_name_cmpfunc (gconstpointer a,
                    gconstpointer b)
{
        return g_strcmp0 ((char *) a,
                          (char *) b);
}

static gboolean
username_in_exclude_list (ActUserManager *manager,
                          const char     *username)
{
        GSList   *found;
        gboolean  ret = FALSE;

        if (manager->priv->exclude_usernames != NULL) {
                found = g_slist_find_custom (manager->priv->exclude_usernames,
                                             username,
                                             match_name_cmpfunc);
                if (found != NULL) {
                        ret = TRUE;
                }
        }

        return ret;
}

static void
add_session_for_user (ActUserManager *manager,
                      ActUser        *user,
                      const char     *ssid,
                      gboolean        is_ours)
{
        g_hash_table_insert (manager->priv->sessions,
                             g_strdup (ssid),
                             g_object_ref (user));

        _act_user_add_session (user, ssid, is_ours);
        g_debug ("ActUserManager: added session for %s", describe_user (user));
}

static void
set_has_multiple_users (ActUserManager *manager,
                        gboolean        has_multiple_users)
{
        if (manager->priv->has_multiple_users != has_multiple_users) {
                manager->priv->has_multiple_users = has_multiple_users;
                g_object_notify (G_OBJECT (manager), "has-multiple-users");
        }
}

static ActUser *
create_new_user (ActUserManager *manager)
{
        ActUser *user;

        user = g_object_new (ACT_TYPE_USER, NULL);

        manager->priv->new_users = g_slist_prepend (manager->priv->new_users, g_object_ref (user));

        g_signal_connect_object (user, "notify::is-loaded", G_CALLBACK (on_new_user_loaded), manager, 0);

        return user;
}

static void
add_user (ActUserManager *manager,
          ActUser        *user)
{
        const char *object_path;

        g_debug ("ActUserManager: tracking user '%s'", act_user_get_user_name (user));
        if (act_user_is_system_account (user)) {
                g_hash_table_insert (manager->priv->system_users_by_name,
                                     g_strdup (act_user_get_user_name (user)),
                                     g_object_ref (user));
        } else {
                g_hash_table_insert (manager->priv->normal_users_by_name,
                                     g_strdup (act_user_get_user_name (user)),
                                     g_object_ref (user));
        }

        object_path = act_user_get_object_path (user);
        if (object_path != NULL) {
                g_hash_table_replace (manager->priv->users_by_object_path,
                                      (gpointer) object_path,
                                      g_object_ref (user));
        }

        g_signal_connect_object (user,
                                 "sessions-changed",
                                 G_CALLBACK (on_user_sessions_changed),
                                 manager, 0);
        g_signal_connect_object (user,
                                 "changed",
                                 G_CALLBACK (on_user_changed),
                                 manager, 0);

        if (manager->priv->is_loaded) {
                g_debug ("ActUserManager: loaded, so emitting user-added signal");
                g_signal_emit (manager, signals[USER_ADDED], 0, user);
        } else {
                g_debug ("ActUserManager: not yet loaded, so not emitting user-added signal");
        }
}

static void
remove_user (ActUserManager *manager,
             ActUser        *user)
{
        g_debug ("ActUserManager: no longer tracking user '%s' (with object path %s)",
                 act_user_get_user_name (user),
                 act_user_get_object_path (user));

        g_object_ref (user);

        g_signal_handlers_disconnect_by_func (user, on_user_changed, manager);
        g_signal_handlers_disconnect_by_func (user, on_user_sessions_changed, manager);
        if (act_user_get_object_path (user) != NULL) {
                g_hash_table_remove (manager->priv->users_by_object_path, act_user_get_object_path (user));
        }
        if (act_user_get_user_name (user) != NULL) {
                g_hash_table_remove (manager->priv->normal_users_by_name, act_user_get_user_name (user));
                g_hash_table_remove (manager->priv->system_users_by_name, act_user_get_user_name (user));

        }

        if (manager->priv->is_loaded) {
                g_debug ("ActUserManager: loaded, so emitting user-removed signal");
                g_signal_emit (manager, signals[USER_REMOVED], 0, user);
        } else {
                g_debug ("ActUserManager: not yet loaded, so not emitting user-removed signal");
        }

        g_debug ("ActUserManager: user '%s' (with object path %s) now removed",
                 act_user_get_user_name (user),
                 act_user_get_object_path (user));
        g_object_unref (user);
}

static void
update_user (ActUserManager *manager,
             ActUser        *user)
{
        const char *username;

        g_debug ("ActUserManager: updating %s", describe_user (user));

        username = act_user_get_user_name (user);
        if (g_hash_table_lookup (manager->priv->system_users_by_name, username) != NULL) {
                if (!act_user_is_system_account (user)) {
                        g_debug ("ActUserManager: %s is no longer a system account, treating as normal user",
                                 describe_user (user));
                        g_hash_table_insert (manager->priv->normal_users_by_name,
                                             g_strdup (act_user_get_user_name (user)),
                                             g_object_ref (user));
                        g_hash_table_remove (manager->priv->system_users_by_name, username);
                        g_signal_emit (manager, signals[USER_ADDED], 0, user);
                }
        } else {
                if (act_user_is_system_account (user)) {
                        g_debug ("ActUserManager: %s is no longer a normal account, treating as system user",
                                 describe_user (user));
                        g_hash_table_insert (manager->priv->system_users_by_name,
                                             g_strdup (act_user_get_user_name (user)),
                                             g_object_ref (user));
                        g_hash_table_remove (manager->priv->normal_users_by_name, username);
                        g_signal_emit (manager, signals[USER_REMOVED], 0, user);
                }
        }
}

static ActUser *
lookup_user_by_name (ActUserManager *manager,
                     const char     *username)
{
        ActUser *user;

        user = g_hash_table_lookup (manager->priv->normal_users_by_name, username);

        if (user == NULL) {
                user = g_hash_table_lookup (manager->priv->system_users_by_name, username);
        }

        return user;
}

static void
on_new_user_loaded (ActUser        *user,
                    GParamSpec     *pspec,
                    ActUserManager *manager)
{
        const char *username;
        ActUser *old_user;

        if (!act_user_is_loaded (user)) {
                g_debug ("ActUserManager: %s loaded function called when not loaded",
                         describe_user (user));
                return;
        }
        g_signal_handlers_disconnect_by_func (user, on_new_user_loaded, manager);

        manager->priv->new_users = g_slist_remove (manager->priv->new_users,
                                                   user);
        manager->priv->new_users_inhibiting_load = g_slist_remove (manager->priv->new_users_inhibiting_load,
                                                                   user);

        username = act_user_get_user_name (user);

        if (username == NULL) {
                const char *object_path;

                object_path = act_user_get_object_path (user);

                if (object_path != NULL) {
                        g_warning ("ActUserManager: %s has no username "
                                   "(object path: %s, uid: %d)",
                                   describe_user (user),
                                   object_path, (int) act_user_get_uid (user));
                } else {
                        g_warning ("ActUserManager: %s has no username (uid: %d)",
                                   describe_user (user),
                                   (int) act_user_get_uid (user));
                }
                g_object_unref (user);
                goto out;
        }

        g_debug ("ActUserManager: %s is now loaded", describe_user (user));

        if (username_in_exclude_list (manager, username)) {
                g_debug ("ActUserManager: excluding user '%s'", username);
                g_object_unref (user);
                goto out;
        }

        old_user = lookup_user_by_name (manager, username);

        /* If username hasn't been added, yet, add it now
         */
        if (old_user == NULL) {
                g_debug ("ActUserManager: %s was not yet known, adding it",
                         describe_user (user));
                add_user (manager, user);
        } else {
                _act_user_load_from_user (old_user, user);
        }

        g_object_unref (user);

out:
        if (manager->priv->new_users_inhibiting_load == NULL) {
                g_debug ("ActUserManager: no pending users, trying to set loaded property");
                maybe_set_is_loaded (manager);
        } else {
                g_debug ("ActUserManager: not all users loaded yet");
        }
}

static ActUser *
find_new_user_with_object_path (ActUserManager *manager,
                                const char     *object_path)
{
        GSList *node;

        g_assert (object_path != NULL);

        for (node = manager->priv->new_users; node != NULL; node = node->next) {
                ActUser *user = ACT_USER (node->data);
                const char *user_object_path = act_user_get_object_path (user);
                if (g_strcmp0 (user_object_path, object_path) == 0) {
                        return user;
                }
        }

        return NULL;
}

static ActUser *
add_new_user_for_object_path (const char     *object_path,
                              ActUserManager *manager)
{
        ActUser *user;

        user = g_hash_table_lookup (manager->priv->users_by_object_path, object_path);

        if (user != NULL) {
                g_debug ("ActUserManager: tracking existing %s with object path %s",
                         describe_user (user), object_path);
                return user;
        }

        user = find_new_user_with_object_path (manager, object_path);

        if (user != NULL) {
                g_debug ("ActUserManager: tracking existing (but very recently added) %s with object path %s",
                         describe_user (user), object_path);
                return user;
        }

        g_debug ("ActUserManager: tracking new user with object path %s", object_path);

        user = create_new_user (manager);
        _act_user_update_from_object_path (user, object_path);

        return user;
}

static void
on_new_user_in_accounts_service (GDBusProxy *proxy,
                                 const char *object_path,
                                 gpointer    user_data)
{
        ActUserManager *manager = ACT_USER_MANAGER (user_data);
        ActUser *user;

        if (!manager->priv->is_loaded) {
                g_debug ("ActUserManager: ignoring new user in accounts service with object path %s since not loaded yet", object_path);
                return;
        }

        g_debug ("ActUserManager: new user in accounts service with object path %s", object_path);
        user = add_new_user_for_object_path (object_path, manager);

        g_object_unref (user);
}

static void
on_user_removed_in_accounts_service (GDBusProxy *proxy,
                                     const char *object_path,
                                     gpointer    user_data)
{
        ActUserManager *manager = ACT_USER_MANAGER (user_data);
        ActUser        *user;
        GSList         *node;

        user = g_hash_table_lookup (manager->priv->users_by_object_path, object_path);

        if (user == NULL) {
                g_debug ("ActUserManager: ignoring untracked user %s", object_path);
                return;
        } else {
                g_debug ("ActUserManager: tracked user %s removed from accounts service", object_path);
        }

        node = g_slist_find (manager->priv->new_users, user);
        if (node != NULL) {
                g_signal_handlers_disconnect_by_func (user, on_new_user_loaded, manager);
                g_object_unref (user);
                manager->priv->new_users = g_slist_delete_link (manager->priv->new_users, node);
        }

        remove_user (manager, user);
}

static void
on_get_current_session_finished (GObject        *object,
                                 GAsyncResult   *result,
                                 gpointer        data)
{
        ConsoleKitManager *proxy = CONSOLE_KIT_MANAGER (object);
        ActUserManager    *manager = data;
        GError            *error = NULL;
        char              *session_id;

        g_assert (manager->priv->seat.state == ACT_USER_MANAGER_SEAT_STATE_GET_SESSION_ID);

        if (!console_kit_manager_call_get_current_session_finish (proxy, &session_id, result, &error)) {
                if (error != NULL) {
                        g_debug ("Failed to identify the current session: %s",
                                 error->message);
                        g_error_free (error);
                } else {
                        g_debug ("Failed to identify the current session");
                }
                unload_seat (manager);

                goto out;
        }

        manager->priv->seat.session_id = session_id;
        manager->priv->seat.state++;

        queue_load_seat_incrementally (manager);

 out:
        g_debug ("ActUserManager: unrefing manager owned by GetCurrentSession request");
        g_object_unref (manager);
}

#ifdef WITH_SYSTEMD
static void
_get_current_systemd_session_id (ActUserManager *manager)
{
        char *session_id;
        int   res;

        res = sd_pid_get_session (0, &session_id);

        if (res == -ENOENT) {
                session_id = NULL;
        } else if (res < 0) {
                g_debug ("Failed to identify the current session: %s",
                         strerror (-res));
                unload_seat (manager);
                return;
        }

        manager->priv->seat.session_id = g_strdup (session_id);
        free (session_id);

        manager->priv->seat.state++;

        queue_load_seat_incrementally (manager);

}
#endif

static void
get_current_session_id (ActUserManager *manager)
{
#ifdef WITH_SYSTEMD
        if (LOGIND_RUNNING()) {
                _get_current_systemd_session_id (manager);
                return;
        }
#endif

        if (manager->priv->ck_manager_proxy == NULL) {
                GError *error = NULL;

                manager->priv->ck_manager_proxy = console_kit_manager_proxy_new_sync (manager->priv->connection,
                                                                                      G_DBUS_PROXY_FLAGS_NONE,
                                                                                      CK_NAME,
                                                                                      CK_MANAGER_PATH,
                                                                                      NULL,
                                                                                      &error);
                if (manager->priv->ck_manager_proxy == NULL) {
                        if (error != NULL) {
                                g_warning ("Failed to create ConsoleKit proxy: %s", error->message);
                                g_error_free (error);
                        } else {
                                g_warning ("Failed to create_ConsoleKit_proxy");
                        }
                        unload_seat (manager);
                        return;
                }
        }

        console_kit_manager_call_get_current_session (manager->priv->ck_manager_proxy, NULL,
                                                      on_get_current_session_finished,
                                                      g_object_ref (manager));
}

static void
unload_new_session (ActUserManagerNewSession *new_session)
{
        ActUserManager *manager;

	/* From here down to the check on pending_calls is idempotent,
	 * like GObject dispose(); it can be called twice if the new session
	 * is unloaded while there are still async calls pending.
	 */

        manager = new_session->manager;

        if (new_session->cancellable != NULL &&
            !g_cancellable_is_cancelled (new_session->cancellable)) {
                g_cancellable_cancel (new_session->cancellable);
                g_object_unref (new_session->cancellable);
                new_session->cancellable = NULL;
        }

        if (new_session->proxy != NULL) {
                g_object_unref (new_session->proxy);
                new_session->proxy = NULL;
        }

        g_free (new_session->x11_display);
        new_session->x11_display = NULL;
        g_free (new_session->id);
        new_session->id = NULL;

        if (manager != NULL) {
                manager->priv->new_sessions = g_slist_remove (manager->priv->new_sessions,
                                                              new_session);

                g_debug ("ActUserManager: unrefing manager owned by new session that's now unloaded");
                new_session->manager = NULL;
                g_object_unref (manager);
        }

        if (new_session->pending_calls != 0) {
                /* don't "finalize" until we run out of pending calls
		 * that have us as their user_data */
                return;
        }

        g_slice_free (ActUserManagerNewSession, new_session);
}

static void
get_proxy_for_new_session (ActUserManagerNewSession *new_session)
{
        GError            *error = NULL;
#ifdef WITH_SYSTEMD
        if (LOGIND_RUNNING()) {
                new_session->state++;
                load_new_session_incrementally (new_session);
                return;
        }
#endif

        new_session->proxy = console_kit_session_proxy_new_sync (new_session->manager->priv->connection,
                                                                 G_DBUS_PROXY_FLAGS_NONE,
                                                                 CK_NAME,
                                                                 new_session->id,
                                                                 NULL,
                                                                 &error);
        if (new_session->proxy == NULL) {
                g_warning ("Failed to connect to the ConsoleKit '%s' object: %s",
                           new_session->id, error->message);
                g_error_free (error);
                unload_new_session (new_session);
                return;
        }

        new_session->state++;

        load_new_session_incrementally (new_session);
}

static void
on_get_unix_user_finished (GObject      *object,
                           GAsyncResult *result,
                           gpointer      data)
{
        ConsoleKitSession *proxy = CONSOLE_KIT_SESSION (object);
        ActUserManagerNewSession *new_session = data;
        GError            *error = NULL;
        guint              uid;

        new_session->pending_calls--;

        if (new_session->cancellable == NULL || g_cancellable_is_cancelled (new_session->cancellable)) {
                unload_new_session (new_session);
                return;
        }

        if (!console_kit_session_call_get_unix_user_finish (proxy, &uid, result, &error)) {
                if (error != NULL) {
                        g_debug ("Failed to get uid of session '%s': %s",
                                 new_session->id, error->message);
                        g_error_free (error);
                } else {
                        g_debug ("Failed to get uid of session '%s'",
                                 new_session->id);
                }
                unload_new_session (new_session);
                return;
        }

        g_debug ("ActUserManager: Found uid of session '%s': %u",
                 new_session->id, uid);

        new_session->uid = (uid_t) uid;
        new_session->state++;

        load_new_session_incrementally (new_session);
}

#ifdef WITH_SYSTEMD
static void
_get_uid_for_new_systemd_session (ActUserManagerNewSession *new_session)
{
        uid_t uid;
        int   res;

        res = sd_session_get_uid (new_session->id, &uid);

        if (res < 0) {
                g_debug ("Failed to get uid of session '%s': %s",
                         new_session->id,
                         strerror (-res));
                unload_new_session (new_session);
                return;
        }

        new_session->uid = uid;
        new_session->state++;

        load_new_session_incrementally (new_session);
}
#endif

static void
get_uid_for_new_session (ActUserManagerNewSession *new_session)
{
#ifdef WITH_SYSTEMD
        if (LOGIND_RUNNING()) {
                _get_uid_for_new_systemd_session (new_session);
                return;
        }
#endif

        g_assert (new_session->proxy != NULL);

        new_session->pending_calls++;
        console_kit_session_call_get_unix_user (new_session->proxy,
                                                new_session->cancellable,
                                                on_get_unix_user_finished,
                                                new_session);
}

static void
on_find_user_by_name_finished (GObject       *object,
                               GAsyncResult  *result,
                               gpointer       data)
{
        AccountsAccounts *proxy = ACCOUNTS_ACCOUNTS (object);
        ActUserManagerFetchUserRequest *request = data;
        GError          *error = NULL;
        char            *user;

        if (!accounts_accounts_call_find_user_by_name_finish (proxy, &user, result, &error)) {
                if (error != NULL) {
                        g_debug ("ActUserManager: Failed to find %s: %s",
                                 request->description, error->message);
                        g_error_free (error);
                } else {
                        g_debug ("ActUserManager: Failed to find %s",
                                 request->description);
                }
                give_up (request->manager, request);
                return;
        }

        g_debug ("ActUserManager: Found object path of %s: %s",
                 request->description, user);
        request->object_path = user;
        request->state++;

        fetch_user_incrementally (request);
}

static void
on_find_user_by_id_finished (GObject       *object,
                             GAsyncResult  *result,
                             gpointer       data)
{
        AccountsAccounts *proxy = ACCOUNTS_ACCOUNTS (object);
        ActUserManagerFetchUserRequest *request = data;
        GError          *error = NULL;
        char            *user;

        if (!accounts_accounts_call_find_user_by_id_finish (proxy, &user, result, &error)) {
                if (error != NULL) {
                        g_debug ("ActUserManager: Failed to find user %lu: %s",
                                 (gulong) request->uid, error->message);
                        g_error_free (error);
                } else {
                        g_debug ("ActUserManager: Failed to find user with id %lu",
                                 (gulong) request->uid);
                }
                give_up (request->manager, request);
                return;
        }

        g_debug ("ActUserManager: Found object path of %s: %s",
                 request->description, user);
        request->object_path = user;
        request->state++;

        fetch_user_incrementally (request);
}

static void
find_user_in_accounts_service (ActUserManager                 *manager,
                               ActUserManagerFetchUserRequest *request)
{
        g_assert (manager->priv->accounts_proxy != NULL);

        g_debug ("ActUserManager: Looking for %s in accounts service",
                 request->description);

        switch (request->type) {
                case ACT_USER_MANAGER_FETCH_USER_FROM_USERNAME_REQUEST:
                    accounts_accounts_call_find_user_by_name (manager->priv->accounts_proxy,
                                                              request->username,
                                                              NULL,
                                                              on_find_user_by_name_finished,
                                                              request);
                    break;
                case ACT_USER_MANAGER_FETCH_USER_FROM_ID_REQUEST:
                    accounts_accounts_call_find_user_by_id (manager->priv->accounts_proxy,
                                                            request->uid,
                                                            NULL,
                                                            on_find_user_by_id_finished,
                                                            request);
                    break;

        }
}

static void
set_is_loaded (ActUserManager *manager,
               gboolean        is_loaded)
{
        if (manager->priv->is_loaded != is_loaded) {
                manager->priv->is_loaded = is_loaded;
                g_object_notify (G_OBJECT (manager), "is-loaded");
        }
}

static void
load_user_paths (ActUserManager       *manager,
                 const char * const * user_paths)
{
        /* We now have a batch of unloaded users that we know about. Once that initial
         * batch is loaded up, we can mark the manager as loaded.
         *
         * (see on_new_user_loaded)
         */
        if (g_strv_length ((char **) user_paths) > 0) {
                int i;

                g_debug ("ActUserManager: ListCachedUsers finished, will set loaded property after list is fully loaded");
                for (i = 0; user_paths[i] != NULL; i++) {
                        ActUser *user;

                        user = add_new_user_for_object_path (user_paths[i], manager);
                        if (!manager->priv->is_loaded) {
                                manager->priv->new_users_inhibiting_load = g_slist_prepend (manager->priv->new_users_inhibiting_load, user);
                        }
                }
        } else {
                g_debug ("ActUserManager: ListCachedUsers finished with empty list, maybe setting loaded property now");
                maybe_set_is_loaded (manager);
        }
}

static void
load_included_usernames (ActUserManager *manager)
{
        GSList *l;

        /* Add users who are specifically included */
        for (l = manager->priv->include_usernames; l != NULL; l = l->next) {
                ActUser *user;

                g_debug ("ActUserManager: Adding included user %s", (char *)l->data);
                /*
                 * The call to act_user_manager_get_user will add the user if it is
                 * valid and not already in the hash.
                 */
                user = act_user_manager_get_user (manager, l->data);
                if (user == NULL) {
                        g_debug ("ActUserManager: unable to lookup user '%s'", (char *)l->data);
                }
        }
}

static void
on_list_cached_users_finished (GObject      *object,
                               GAsyncResult *result,
                               gpointer      data)
{
        AccountsAccounts *proxy = ACCOUNTS_ACCOUNTS (object);
        ActUserManager   *manager = data;
        gchar           **user_paths;
        GError           *error = NULL;

        manager->priv->listing_cached_users = FALSE;

        if (!accounts_accounts_call_list_cached_users_finish (proxy, &user_paths, result, &error)) {
                g_debug ("ActUserManager: ListCachedUsers failed: %s", error->message);
                g_error_free (error);

                g_object_unref (manager->priv->accounts_proxy);
                manager->priv->accounts_proxy = NULL;

                g_debug ("ActUserManager: unrefing manager owned by failed ListCachedUsers call");
                g_object_unref (manager);
                return;
        }

        load_user_paths (manager, (const char * const *) user_paths);
        g_strfreev (user_paths);

        load_included_usernames (manager);

        g_debug ("ActUserManager: unrefing manager owned by finished ListCachedUsers call");
        g_object_unref (manager);
}

static void
on_get_x11_display_finished (GObject      *object,
                             GAsyncResult *result,
                             gpointer      data)
{
        ConsoleKitSession *proxy = CONSOLE_KIT_SESSION (object);
        ActUserManagerNewSession *new_session = data;
        GError            *error = NULL;
        char              *x11_display;

        new_session->pending_calls--;

        if (new_session->cancellable == NULL || g_cancellable_is_cancelled (new_session->cancellable)) {
                unload_new_session (new_session);
                return;
        }

        if (!console_kit_session_call_get_x11_display_finish (proxy, &x11_display, result, &error)) {
                if (error != NULL) {
                        g_debug ("Failed to get the x11 display of session '%s': %s",
                                 new_session->id, error->message);
                        g_error_free (error);
                } else {
                        g_debug ("Failed to get the x11 display of session '%s'",
                                 new_session->id);
                }
                unload_new_session (new_session);
                return;
        }

        g_debug ("ActUserManager: Found x11 display of session '%s': %s",
                 new_session->id, x11_display);

        new_session->x11_display = x11_display;
        new_session->state++;

        load_new_session_incrementally (new_session);
}

#ifdef WITH_SYSTEMD
static void
_get_x11_display_for_new_systemd_session (ActUserManagerNewSession *new_session)
{
        char *session_type;
        char *x11_display;
        int   res;

        res = sd_session_get_type (new_session->id,
                                   &session_type);

        if (res < 0) {
                g_debug ("ActUserManager: Failed to get the type of session '%s': %s",
                         new_session->id,
                         strerror (-res));
                unload_new_session (new_session);
                return;
        }

        if (g_strcmp0 (session_type, "x11") != 0) {
                g_debug ("ActUserManager: (mostly) ignoring %s session '%s' since it's not graphical",
                         session_type,
                         new_session->id);
                free (session_type);
                x11_display = NULL;
                goto done;
        }
        free (session_type);

        res = sd_session_get_display (new_session->id,
                                      &x11_display);

        if (res < 0) {
                g_debug ("ActUserManager: Failed to get the x11 display of session '%s': %s",
                         new_session->id,
                         strerror (-res));
                g_debug ("ActUserManager: Treating X11 display as blank");
                x11_display = strdup ("");
        } else {
                g_debug ("ActUserManager: Found x11 display of session '%s': %s",
                         new_session->id, x11_display);
        }

 done:
        new_session->x11_display = g_strdup (x11_display);
        free (x11_display);
        new_session->state++;

        load_new_session_incrementally (new_session);
}
#endif

static void
get_x11_display_for_new_session (ActUserManagerNewSession *new_session)
{
#ifdef WITH_SYSTEMD
        if (LOGIND_RUNNING()) {
                _get_x11_display_for_new_systemd_session (new_session);
                return;
        }
#endif

        g_assert (new_session->proxy != NULL);

        new_session->pending_calls++;
        console_kit_session_call_get_x11_display (new_session->proxy,
                                                  new_session->cancellable,
                                                  on_get_x11_display_finished,
                                                  new_session);
}

static void
maybe_add_new_session (ActUserManagerNewSession *new_session)
{
        ActUserManager *manager;
        ActUser        *user;
        gboolean        is_ours;

        manager = ACT_USER_MANAGER (new_session->manager);

        is_ours = TRUE;

        if (new_session->x11_display == NULL) {
                g_debug ("AcUserManager: (mostly) ignoring session '%s' since it's not graphical",
                         new_session->id);
                is_ours = FALSE;
        } else if (session_is_login_window (manager, new_session->id)) {
                new_session->state = ACT_USER_MANAGER_NEW_SESSION_STATE_LOADED;
                unload_new_session (new_session);
                return;
        } else if (!session_is_on_our_seat (manager, new_session->id)) {
                is_ours = FALSE;
        }

        user = act_user_manager_get_user_by_id (manager, new_session->uid);
        if (user == NULL) {
                unload_new_session (new_session);
                return;
        }

        add_session_for_user (manager, user, new_session->id, is_ours);

        /* if we haven't yet gotten the login frequency
           then at least add one because the session exists */
        if (act_user_get_login_frequency (user) == 0) {
                _act_user_update_login_frequency (user, 1);
        }
}

static void
load_new_session (ActUserManager *manager,
                  const char     *session_id)
{
        ActUserManagerNewSession *new_session;

        new_session = g_slice_new0 (ActUserManagerNewSession);

        new_session->manager = g_object_ref (manager);
        new_session->id = g_strdup (session_id);
        new_session->state = ACT_USER_MANAGER_NEW_SESSION_STATE_UNLOADED + 1;
        new_session->cancellable = g_cancellable_new ();

        manager->priv->new_sessions = g_slist_prepend (manager->priv->new_sessions,
                                                       new_session);
        load_new_session_incrementally (new_session);
}

static void
seat_session_added (GDBusProxy     *seat_proxy,
                    const char     *session_id,
                    ActUserManager *manager)
{
        g_debug ("ActUserManager: Session added: %s", session_id);

        load_new_session (manager, session_id);
}

static gint
match_new_session_cmpfunc (gconstpointer a,
                           gconstpointer b)
{
        ActUserManagerNewSession *new_session;
        const char               *session_id;

        new_session = (ActUserManagerNewSession *) a;
        session_id = (const char *) b;

        return strcmp (new_session->id, session_id);
}

static void
_remove_session (ActUserManager *manager,
                 const char     *session_id)
{
        ActUser       *user;
        GSList        *found;

        g_debug ("ActUserManager: Session removed: %s", session_id);

        found = g_slist_find_custom (manager->priv->new_sessions,
                                     session_id,
                                     match_new_session_cmpfunc);

        if (found != NULL) {
                ActUserManagerNewSession *new_session;

                new_session = (ActUserManagerNewSession *) found->data;

                if (new_session->state > ACT_USER_MANAGER_NEW_SESSION_STATE_GET_X11_DISPLAY) {
                        g_debug ("ActUserManager: New session for uid %d on "
                                 "x11 display %s removed before fully loading",
                                 (int) new_session->uid, new_session->x11_display);
                } else if (new_session->state > ACT_USER_MANAGER_NEW_SESSION_STATE_GET_UID) {
                        g_debug ("ActUserManager: New session for uid %d "
                                 "removed before fully loading",
                                 (int) new_session->uid);
                } else {
                        g_debug ("ActUserManager: New session removed "
                                 "before fully loading");
                }
                unload_new_session (new_session);
                return;
        }

        /* since the session object may already be gone
         * we can't query CK directly */

        user = g_hash_table_lookup (manager->priv->sessions, session_id);

        if (user == NULL) {
                /* nothing to do */
                return;
        }

        g_debug ("ActUserManager: Session removed for %s", describe_user (user));
        _act_user_remove_session (user, session_id);
        g_hash_table_remove (manager->priv->sessions, session_id);
}

static void
seat_session_removed (GDBusProxy     *seat_proxy,
                      const char     *session_id,
                      ActUserManager *manager)
{
        _remove_session (manager, session_id);
}

#ifdef WITH_SYSTEMD

static gboolean
_session_recognized (ActUserManager *manager,
                     const char     *session_id)
{
        GSList *node;

        if (g_hash_table_contains (manager->priv->sessions,
                                   session_id)) {
                return TRUE;
        }

        node = manager->priv->new_sessions;
        while (node != NULL) {
                ActUserManagerNewSession *new_session = node->data;

                if (g_strcmp0 (new_session->id, session_id) == 0) {
                        return TRUE;
                }

                node = node->next;
        }
        return FALSE;
}

static void
_add_systemd_session (ActUserManager *manager,
                      const char     *session_id)
{
        load_new_session (manager, session_id);
}

static void
_add_new_systemd_sessions (ActUserManager *manager,
                           GHashTable     *systemd_sessions)
{
        GHashTableIter iter;
        gpointer key, value;

        g_hash_table_iter_init (&iter, systemd_sessions);
        while (g_hash_table_iter_next (&iter, &key, &value)) {
                char *session_id = key;

                if (!_session_recognized (manager, session_id)) {
                        _add_systemd_session (manager, session_id);
                }
        }
}

static void
_remove_systemd_session (ActUserManager *manager,
                         const char     *session_id)
{
        _remove_session (manager, session_id);
}

static void
_remove_stale_systemd_sessions (ActUserManager *manager,
                                GHashTable     *systemd_sessions)
{
        GHashTableIter iter;
        gpointer key, value;
        GSList *node, *sessions_to_remove;

        sessions_to_remove = NULL;
        g_hash_table_iter_init (&iter, manager->priv->sessions);
        while (g_hash_table_iter_next (&iter, &key, &value)) {
                char *session_id = key;

                if (g_hash_table_contains (systemd_sessions, session_id)) {
                        continue;
                }

                sessions_to_remove = g_slist_prepend (sessions_to_remove, session_id);
        }

        node = manager->priv->new_sessions;
        while (node != NULL) {
                ActUserManagerNewSession *new_session = node->data;
                node = node->next;

                if (g_hash_table_contains (systemd_sessions, new_session->id)) {
                        continue;
                }

                sessions_to_remove = g_slist_prepend (sessions_to_remove, new_session->id);
        }

        node = sessions_to_remove;
        while (node != NULL) {
                char *session_id = node->data;
                GSList *next_node = node->next;

                _remove_systemd_session (manager, session_id);

                node = next_node;
        }

        g_slist_free (sessions_to_remove);
}

#ifdef WITH_SYSTEMD
static void
reload_systemd_sessions (ActUserManager *manager)
{
        int         res;
        int         i;
        char       **sessions;
        GHashTable  *systemd_sessions;
        char        *state;
        gboolean     is_closing;


        res = sd_get_sessions (&sessions);

        if (res < 0) {
                g_debug ("Failed to determine sessions: %s", strerror (-res));
                return;
        }

        systemd_sessions = g_hash_table_new (g_str_hash,
                                             g_str_equal);

        if (sessions != NULL) {
                for (i = 0; sessions[i] != NULL; i ++) {
                        char *session_class;

                        res = sd_session_get_state (sessions[i], &state);

                        if (res < 0) {
                                g_debug ("Failed to determine state of session %s: %s", sessions[i], strerror (-res));
                                continue;
                        }

                        is_closing = g_strcmp0 (state, "closing") == 0;
                        free (state);

                        if (is_closing) {
                                continue;
                        }

                        session_class = NULL;
                        res = sd_session_get_class (sessions[i], &session_class);

                        if (res < 0) {
                                g_debug ("Failed to determine class of session %s: %s", sessions[i], strerror (-res));
                                continue;
                        }

                        if (g_strcmp0 (session_class, "user") != 0) {
                                g_debug ("Ignoring non-user session %s (class %s)", sessions[i], session_class);
                                free (session_class);
                                continue;
                        }
                        free (session_class);

                        g_hash_table_insert (systemd_sessions,
                                             sessions[i], NULL);
                }

        }

        _add_new_systemd_sessions (manager, systemd_sessions);
        _remove_stale_systemd_sessions (manager, systemd_sessions);
        g_hash_table_unref (systemd_sessions);

        if (sessions != NULL) {
                for (i = 0; sessions[i]; i ++) {
                        free (sessions[i]);
                }

                free (sessions);
        }
}

#endif
static gboolean
on_session_monitor_event (GPollableInputStream *stream,
                          ActUserManager       *manager)
{
        sd_login_monitor_flush (manager->priv->seat.session_monitor);
        reload_systemd_sessions (manager);
        return TRUE;
}

static void
_monitor_for_systemd_session_changes (ActUserManager *manager)
{
        int res;
        int fd;
        GSource *source;

        res = sd_login_monitor_new ("session", &manager->priv->seat.session_monitor);

        if (res < 0) {
                g_warning ("Failed to monitor logind session changes: %s",
                           strerror (-res));
                unload_seat (manager);
                return;
        }

        fd = sd_login_monitor_get_fd (manager->priv->seat.session_monitor);

        manager->priv->seat.session_monitor_stream = g_unix_input_stream_new (fd, FALSE);
        source = g_pollable_input_stream_create_source (G_POLLABLE_INPUT_STREAM (manager->priv->seat.session_monitor_stream),
                                                        NULL);
        g_source_set_callback (source,
                               (GSourceFunc)
                               on_session_monitor_event,
                               manager,
                               NULL);
        manager->priv->seat.session_monitor_source_id = g_source_attach (source, NULL);
        g_source_unref (source);
}
#endif

static void
get_seat_proxy (ActUserManager *manager)
{
        GError *error = NULL;

#ifdef WITH_SYSTEMD
        if (LOGIND_RUNNING()) {
                _monitor_for_systemd_session_changes (manager);
                manager->priv->seat.state++;
                return;
        }
#endif

        g_assert (manager->priv->seat.seat_proxy == NULL);

        manager->priv->seat.seat_proxy = console_kit_seat_proxy_new_sync (manager->priv->connection,
                                                                          G_DBUS_PROXY_FLAGS_NONE,
                                                                          CK_NAME,
                                                                          manager->priv->seat.id,
                                                                          NULL,
                                                                          &error);
        if (manager->priv->seat.seat_proxy == NULL) {
                if (error != NULL) {
                        g_warning ("Failed to connect to the ConsoleKit seat object: %s",
                                   error->message);
                        g_error_free (error);
                } else {
                        g_warning ("Failed to connect to the ConsoleKit seat object");
                }
                unload_seat (manager);
                return;
        }

        g_signal_connect (manager->priv->seat.seat_proxy,
                          "session-added",
                          G_CALLBACK (seat_session_added),
                          manager);
        g_signal_connect (manager->priv->seat.seat_proxy,
                          "session-removed",
                          G_CALLBACK (seat_session_removed),
                          manager);
        manager->priv->seat.state++;
}

static void
on_console_kit_session_proxy_gotten (GObject *object, GAsyncResult *result, gpointer user_data)
{
        ActUserManager *manager = user_data;
        GError *error = NULL;

        g_debug ("on_console_kit_session_proxy_gotten");

        manager->priv->seat.session_proxy = console_kit_session_proxy_new_finish (result, &error);

        if (manager->priv->seat.session_proxy == NULL) {
                if (error != NULL) {
                        g_warning ("Failed to connect to the ConsoleKit session object: %s",
                                   error->message);
                        g_error_free (error);
                } else {
                        g_warning ("Failed to connect to the ConsoleKit session object");
                }
                unload_seat (manager);

                goto out;
        }

        manager->priv->seat.state++;
        load_seat_incrementally (manager);

 out:
        g_debug ("ActUserManager: unrefing manager owned by ConsoleKit proxy getter");
        g_object_unref (manager);
}

static void
get_session_proxy (ActUserManager *manager)
{
#ifdef WITH_SYSTEMD
        if (LOGIND_RUNNING()) {
                manager->priv->seat.state++;
                queue_load_seat_incrementally (manager);
                return;
        }
#endif

        g_debug ("ActUserManager: fetching user proxy");

        g_assert (manager->priv->seat.session_proxy == NULL);

        console_kit_session_proxy_new (manager->priv->connection,
                                       G_DBUS_PROXY_FLAGS_NONE,
                                       CK_NAME,
                                       manager->priv->seat.session_id,
                                       NULL,
                                       on_console_kit_session_proxy_gotten,
                                       g_object_ref (manager));
}

static void
unload_seat (ActUserManager *manager)
{
        manager->priv->seat.state = ACT_USER_MANAGER_SEAT_STATE_UNLOADED;

        if (manager->priv->seat.seat_proxy != NULL) {
                g_object_unref (manager->priv->seat.seat_proxy);
                manager->priv->seat.seat_proxy = NULL;
        }

        if (manager->priv->seat.session_proxy != NULL) {
                g_object_unref (manager->priv->seat.session_proxy);
                manager->priv->seat.session_proxy = NULL;
        }

        g_free (manager->priv->seat.id);
        manager->priv->seat.id = NULL;

        g_free (manager->priv->seat.session_id);
        manager->priv->seat.session_id = NULL;

        g_debug ("ActUserManager: seat unloaded, so trying to set loaded property");
        maybe_set_is_loaded (manager);
}

static void
load_new_session_incrementally (ActUserManagerNewSession *new_session)
{
        switch (new_session->state) {
        case ACT_USER_MANAGER_NEW_SESSION_STATE_GET_PROXY:
                get_proxy_for_new_session (new_session);
                break;
        case ACT_USER_MANAGER_NEW_SESSION_STATE_GET_UID:
                get_uid_for_new_session (new_session);
                break;
        case ACT_USER_MANAGER_NEW_SESSION_STATE_GET_X11_DISPLAY:
                get_x11_display_for_new_session (new_session);
                break;
        case ACT_USER_MANAGER_NEW_SESSION_STATE_MAYBE_ADD:
                maybe_add_new_session (new_session);
                break;
        case ACT_USER_MANAGER_NEW_SESSION_STATE_LOADED:
                break;
        default:
                g_assert_not_reached ();
        }
}

static void
free_fetch_user_request (ActUserManagerFetchUserRequest *request)
{
        ActUserManager *manager;

        manager = request->manager;

        g_object_set_data (G_OBJECT (request->user), "fetch-user-request", NULL);

        manager->priv->fetch_user_requests = g_slist_remove (manager->priv->fetch_user_requests, request);
        if (request->type == ACT_USER_MANAGER_FETCH_USER_FROM_USERNAME_REQUEST) {
                g_free (request->username);
        }

        g_free (request->object_path);
        g_free (request->description);
        g_debug ("ActUserManager: unrefing manager owned by fetch user request");
        g_object_unref (manager);

        g_slice_free (ActUserManagerFetchUserRequest, request);
}

static void
give_up (ActUserManager                 *manager,
         ActUserManagerFetchUserRequest *request)
{
        if (request->type == ACT_USER_MANAGER_FETCH_USER_FROM_USERNAME_REQUEST)
                g_debug ("ActUserManager: failed to load user %s", request->username);
        else
                g_debug ("ActUserManager: failed to load user %lu", (gulong) request->uid);

        request->state = ACT_USER_MANAGER_GET_USER_STATE_UNFETCHED;

        if (request->user)
                _act_user_update_as_nonexistent (request->user);
}

static void
on_user_manager_maybe_ready_for_request (ActUserManager                 *manager,
                                         GParamSpec                     *pspec,
                                         ActUserManagerFetchUserRequest *request)
{
        if (!manager->priv->is_loaded) {
                return;
        }

        g_debug ("ActUserManager: user manager now loaded, proceeding with fetch user request for %s",
                 request->description);

        g_signal_handlers_disconnect_by_func (manager, on_user_manager_maybe_ready_for_request, request);

        request->state++;
        fetch_user_incrementally (request);
}

static void
fetch_user_incrementally (ActUserManagerFetchUserRequest *request)
{
        ActUserManager *manager;

        g_debug ("ActUserManager: finding %s state %d",
                 request->description, request->state);
        manager = request->manager;
        switch (request->state) {
        case ACT_USER_MANAGER_GET_USER_STATE_WAIT_FOR_LOADED:
                if (manager->priv->is_loaded) {
                        request->state++;
                        fetch_user_incrementally (request);
                } else {
                        g_debug ("ActUserManager: waiting for user manager to load before finding %s",
                                 request->description);
                        g_signal_connect (manager, "notify::is-loaded",
                                          G_CALLBACK (on_user_manager_maybe_ready_for_request), request);

                }
                break;

        case ACT_USER_MANAGER_GET_USER_STATE_ASK_ACCOUNTS_SERVICE:
                if (manager->priv->accounts_proxy == NULL) {
                        give_up (manager, request);
                } else {
                        find_user_in_accounts_service (manager, request);
                }
                break;
        case ACT_USER_MANAGER_GET_USER_STATE_FETCHED:
                g_debug ("ActUserManager: %s fetched", request->description);
                _act_user_update_from_object_path (request->user, request->object_path);
                break;
        case ACT_USER_MANAGER_GET_USER_STATE_UNFETCHED:
                g_debug ("ActUserManager: %s was not fetched", request->description);
                break;
        default:
                g_assert_not_reached ();
        }

        if (request->state == ACT_USER_MANAGER_GET_USER_STATE_FETCHED  ||
            request->state == ACT_USER_MANAGER_GET_USER_STATE_UNFETCHED) {
                g_debug ("ActUserManager: finished handling request for %s",
                         request->description);
                free_fetch_user_request (request);
        }
}

static void
fetch_user_with_username_from_accounts_service (ActUserManager *manager,
                                                ActUser        *user,
                                                const char     *username)
{
        ActUserManagerFetchUserRequest *request;

        request = g_slice_new0 (ActUserManagerFetchUserRequest);

        request->manager = g_object_ref (manager);
        request->type = ACT_USER_MANAGER_FETCH_USER_FROM_USERNAME_REQUEST;
        request->username = g_strdup (username);
        request->user = user;
        request->state = ACT_USER_MANAGER_GET_USER_STATE_UNFETCHED + 1;
        request->description = g_strdup_printf ("user '%s'", request->username);

        manager->priv->fetch_user_requests = g_slist_prepend (manager->priv->fetch_user_requests,
                                                              request);
        g_object_set_data (G_OBJECT (user), "fetch-user-request", request);
        fetch_user_incrementally (request);
}

static void
fetch_user_with_id_from_accounts_service (ActUserManager *manager,
                                          ActUser        *user,
                                          uid_t           id)
{
        ActUserManagerFetchUserRequest *request;

        request = g_slice_new0 (ActUserManagerFetchUserRequest);

        request->manager = g_object_ref (manager);
        request->type = ACT_USER_MANAGER_FETCH_USER_FROM_ID_REQUEST;
        request->uid = id;
        request->user = user;
        request->state = ACT_USER_MANAGER_GET_USER_STATE_UNFETCHED + 1;
        request->description = g_strdup_printf ("user with id %lu", (gulong) request->uid);

        manager->priv->fetch_user_requests = g_slist_prepend (manager->priv->fetch_user_requests,
                                                              request);
        g_object_set_data (G_OBJECT (user), "fetch-user-request", request);
        fetch_user_incrementally (request);
}

/**
 * act_user_manager_get_user:
 * @manager: the manager to query.
 * @username: the login name of the user to get.
 *
 * Retrieves a pointer to the #ActUser object for the login @username
 * from @manager. Trying to use this object before its
 * #ActUser:is-loaded property is %TRUE will result in undefined
 * behavior.
 *
 * Returns: (transfer none): #ActUser object
 **/
ActUser *
act_user_manager_get_user (ActUserManager *manager,
                           const char     *username)
{
        ActUser *user;

        g_return_val_if_fail (ACT_IS_USER_MANAGER (manager), NULL);
        g_return_val_if_fail (username != NULL && username[0] != '\0', NULL);

        user = lookup_user_by_name (manager, username);

        /* if we don't have it loaded try to load it now */
        if (user == NULL) {
                g_debug ("ActUserManager: trying to track new user with username %s", username);
                user = create_new_user (manager);

                if (manager->priv->accounts_proxy != NULL) {
                        fetch_user_with_username_from_accounts_service (manager, user, username);
                }
        }

        return user;
}

/**
 * act_user_manager_get_user_by_id:
 * @manager: the manager to query.
 * @id: the uid of the user to get.
 *
 * Retrieves a pointer to the #ActUser object for the user with the
 * given uid from @manager. Trying to use this object before its
 * #ActUser:is-loaded property is %TRUE will result in undefined
 * behavior.
 *
 * Returns: (transfer none): #ActUser object
 */
ActUser *
act_user_manager_get_user_by_id (ActUserManager *manager,
                                 uid_t           id)
{
        ActUser *user;
        gchar  *object_path;

        g_return_val_if_fail (ACT_IS_USER_MANAGER (manager), NULL);

        object_path = g_strdup_printf ("/org/freedesktop/Accounts/User%lu", (gulong) id);
        user = g_hash_table_lookup (manager->priv->users_by_object_path, object_path);
        g_free (object_path);

        if (user != NULL) {
                return g_object_ref (user);
        } else {
                g_debug ("ActUserManager: trying to track new user with uid %lu", (gulong) id);
                user = create_new_user (manager);

                if (manager->priv->accounts_proxy != NULL) {
                        fetch_user_with_id_from_accounts_service (manager, user, id);
                }
        }

        return user;
}

static void
listify_hash_values_hfunc (gpointer key,
                           gpointer value,
                           gpointer user_data)
{
        GSList **list = user_data;

        *list = g_slist_prepend (*list, value);
}

/**
 * act_user_manager_list_users:
 * @manager: a #ActUserManager
 *
 * Get a list of system user accounts
 *
 * Returns: (element-type ActUser) (transfer container): List of #ActUser objects
 */
GSList *
act_user_manager_list_users (ActUserManager *manager)
{
        GSList *retval;

        g_return_val_if_fail (ACT_IS_USER_MANAGER (manager), NULL);

        retval = NULL;
        g_hash_table_foreach (manager->priv->normal_users_by_name, listify_hash_values_hfunc, &retval);

        return g_slist_sort (retval, (GCompareFunc) act_user_collate);
}

static void
maybe_set_is_loaded (ActUserManager *manager)
{
        if (manager->priv->is_loaded) {
                g_debug ("ActUserManager: already loaded, so not setting loaded property");
                return;
        }

        if (manager->priv->getting_sessions) {
                g_debug ("ActUserManager: GetSessions call pending, so not setting loaded property");
                return;
        }

        if (manager->priv->listing_cached_users) {
                g_debug ("ActUserManager: Listing cached users, so not setting loaded property");
                return;
        }

        if (manager->priv->new_users_inhibiting_load != NULL) {
                g_debug ("ActUserManager: Loading new users, so not setting loaded property");
                return;
        }

        /* Don't set is_loaded yet unless the seat is already loaded
         * or failed to load.
         */
        if (manager->priv->seat.state == ACT_USER_MANAGER_SEAT_STATE_LOADED) {
                g_debug ("ActUserManager: Seat loaded, so now setting loaded property");
        } else if (manager->priv->seat.state == ACT_USER_MANAGER_SEAT_STATE_UNLOADED) {
                g_debug ("ActUserManager: Seat wouldn't load, so giving up on it and setting loaded property");
        } else {
                g_debug ("ActUserManager: Seat still actively loading, so not setting loaded property");
                return;
        }

        set_is_loaded (manager, TRUE);
}


static GSList *
slist_deep_copy (const GSList *list)
{
        GSList *retval;
        GSList *l;

        if (list == NULL)
                return NULL;

        retval = g_slist_copy ((GSList *) list);
        for (l = retval; l != NULL; l = l->next) {
                l->data = g_strdup (l->data);
        }

        return retval;
}

static void
on_get_sessions_finished (GObject      *object,
                          GAsyncResult *result,
                          gpointer      data)
{
        ConsoleKitSeat *proxy = CONSOLE_KIT_SEAT (object);
        ActUserManager *manager = data;
        GError         *error = NULL;
        gchar         **session_ids;
        int             i;

        if (!console_kit_seat_call_get_sessions_finish (proxy, &session_ids, result, &error)) {
                if (error != NULL) {
                        g_warning ("unable to determine sessions for seat: %s",
                                   error->message);
                        g_error_free (error);
                } else {
                        g_warning ("unable to determine sessions for seat");
                }

                goto out;
        }

        manager->priv->getting_sessions = FALSE;
        for (i = 0; session_ids[i] != NULL; i++) {
                load_new_session (manager, session_ids[i]);
        }
        g_strfreev (session_ids);

        g_debug ("ActUserManager: GetSessions call finished, so trying to set loaded property");
        maybe_set_is_loaded (manager);

 out:
        g_debug ("ActUserManager: unrefing manager owned by GetSessions request");
        g_object_unref (manager);
}

static void
load_console_kit_sessions (ActUserManager *manager)
{
        if (manager->priv->seat.seat_proxy == NULL) {
                g_debug ("ActUserManager: no seat proxy; can't load sessions");
                return;
        }

        manager->priv->getting_sessions = TRUE;
        console_kit_seat_call_get_sessions (manager->priv->seat.seat_proxy,
                                            NULL,
                                            on_get_sessions_finished,
                                            g_object_ref (manager));
}

static void
load_sessions (ActUserManager *manager)
{
#ifdef WITH_SYSTEMD
        if (LOGIND_RUNNING()) {
                reload_systemd_sessions (manager);
                maybe_set_is_loaded (manager);
                return;
        }
#endif
        load_console_kit_sessions (manager);
}

static void
load_users (ActUserManager *manager)
{
        g_assert (manager->priv->accounts_proxy != NULL);
        g_debug ("ActUserManager: calling 'ListCachedUsers'");

        accounts_accounts_call_list_cached_users (manager->priv->accounts_proxy,
                                                  NULL,
                                                  on_list_cached_users_finished,
                                                  g_object_ref (manager));
        manager->priv->listing_cached_users = TRUE;
}

static gboolean
load_seat_incrementally (ActUserManager *manager)
{
        manager->priv->seat.load_idle_id = 0;

        switch (manager->priv->seat.state) {
        case ACT_USER_MANAGER_SEAT_STATE_GET_SESSION_ID:
                get_current_session_id (manager);
                break;
        case ACT_USER_MANAGER_SEAT_STATE_GET_SESSION_PROXY:
                get_session_proxy (manager);
                break;
        case ACT_USER_MANAGER_SEAT_STATE_GET_ID:
                get_seat_id_for_current_session (manager);
                break;
        case ACT_USER_MANAGER_SEAT_STATE_GET_SEAT_PROXY:
                get_seat_proxy (manager);
                break;
        case ACT_USER_MANAGER_SEAT_STATE_LOADED:
                g_debug ("ActUserManager: Seat loading sequence complete");
                break;
        default:
                g_assert_not_reached ();
        }

        if (manager->priv->seat.state == ACT_USER_MANAGER_SEAT_STATE_LOADED) {
                load_sessions (manager);
        }

        maybe_set_is_loaded (manager);

        return FALSE;
}

static gboolean
load_idle (ActUserManager *manager)
{
        /* The order below is important: load_seat_incrementally might
           set "is-loaded" immediately and we thus need to call
           load_users before it.
        */
        load_users (manager);
        manager->priv->seat.state = ACT_USER_MANAGER_SEAT_STATE_UNLOADED + 1;
        load_seat_incrementally (manager);
        manager->priv->load_id = 0;

        return FALSE;
}

static void
queue_load_seat_and_users (ActUserManager *manager)
{
        if (manager->priv->load_id > 0) {
                return;
        }

        manager->priv->load_id = g_idle_add ((GSourceFunc)load_idle, manager);
}

static void
act_user_manager_get_property (GObject        *object,
                               guint           prop_id,
                               GValue         *value,
                               GParamSpec     *pspec)
{
        ActUserManager *manager;

        manager = ACT_USER_MANAGER (object);

        switch (prop_id) {
        case PROP_IS_LOADED:
                g_value_set_boolean (value, manager->priv->is_loaded);
                break;
        case PROP_HAS_MULTIPLE_USERS:
                g_value_set_boolean (value, manager->priv->has_multiple_users);
                break;
        case PROP_INCLUDE_USERNAMES_LIST:
                g_value_set_pointer (value, manager->priv->include_usernames);
                break;
        case PROP_EXCLUDE_USERNAMES_LIST:
                g_value_set_pointer (value, manager->priv->exclude_usernames);
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void
set_include_usernames (ActUserManager *manager,
                       GSList         *list)
{
        if (manager->priv->include_usernames != NULL) {
                g_slist_foreach (manager->priv->include_usernames, (GFunc) g_free, NULL);
                g_slist_free (manager->priv->include_usernames);
        }
        manager->priv->include_usernames = slist_deep_copy (list);
}

static void
set_exclude_usernames (ActUserManager *manager,
                       GSList         *list)
{
        if (manager->priv->exclude_usernames != NULL) {
                g_slist_foreach (manager->priv->exclude_usernames, (GFunc) g_free, NULL);
                g_slist_free (manager->priv->exclude_usernames);
        }
        manager->priv->exclude_usernames = slist_deep_copy (list);
}

static void
act_user_manager_set_property (GObject        *object,
                               guint           prop_id,
                               const GValue   *value,
                               GParamSpec     *pspec)
{
        ActUserManager *self;

        self = ACT_USER_MANAGER (object);

        switch (prop_id) {
        case PROP_INCLUDE_USERNAMES_LIST:
                set_include_usernames (self, g_value_get_pointer (value));
                break;
        case PROP_EXCLUDE_USERNAMES_LIST:
                set_exclude_usernames (self, g_value_get_pointer (value));
                break;
        case PROP_HAS_MULTIPLE_USERS:
                set_has_multiple_users (self, g_value_get_boolean (value));
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void
act_user_manager_class_init (ActUserManagerClass *klass)
{
        GObjectClass   *object_class = G_OBJECT_CLASS (klass);

        object_class->finalize = act_user_manager_finalize;
        object_class->get_property = act_user_manager_get_property;
        object_class->set_property = act_user_manager_set_property;

        g_object_class_install_property (object_class,
                                         PROP_IS_LOADED,
                                         g_param_spec_boolean ("is-loaded",
                                                               "Is loaded",
                                                               "Determines whether or not the manager object is loaded and ready to read from.",
                                                               FALSE,
                                                               G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
        g_object_class_install_property (object_class,
                                         PROP_HAS_MULTIPLE_USERS,
                                         g_param_spec_boolean ("has-multiple-users",
                                                               "Has multiple users",
                                                               "Whether more than one normal user is present",
                                                               FALSE,
                                                               G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
        g_object_class_install_property (object_class,
                                         PROP_INCLUDE_USERNAMES_LIST,
                                         g_param_spec_pointer ("include-usernames-list",
                                                               "Include usernames list",
                                                               "Usernames who are specifically included",
                                                               G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

        g_object_class_install_property (object_class,
                                         PROP_EXCLUDE_USERNAMES_LIST,
                                         g_param_spec_pointer ("exclude-usernames-list",
                                                               "Exclude usernames list",
                                                               "Usernames who are specifically excluded",
                                                               G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

        /**
         * ActUserManager::user-added:
         * @gobject: the object which received the signal
         * @user: the #ActUser that was added
         *
         * Emitted when a user is added to the user manager.
         */
        signals [USER_ADDED] =
                g_signal_new ("user-added",
                              G_TYPE_FROM_CLASS (klass),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (ActUserManagerClass, user_added),
                              NULL, NULL,
                              g_cclosure_marshal_VOID__OBJECT,
                              G_TYPE_NONE, 1, ACT_TYPE_USER);
        /**
         * ActUserManager::user-removed:
         * @gobject: the object which received the signal
         * @user: the #ActUser that was removed
         *
         * Emitted when a user is removed from the user manager.
         */
        signals [USER_REMOVED] =
                g_signal_new ("user-removed",
                              G_TYPE_FROM_CLASS (klass),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (ActUserManagerClass, user_removed),
                              NULL, NULL,
                              g_cclosure_marshal_VOID__OBJECT,
                              G_TYPE_NONE, 1, ACT_TYPE_USER);
        /**
         * ActUserManager::user-is-logged-in-changed:
         * @gobject: the object which received the signal
         * @user: the #ActUser that changed login status
         *
         * One of the users has logged in or out.
         */
        signals [USER_IS_LOGGED_IN_CHANGED] =
                g_signal_new ("user-is-logged-in-changed",
                              G_TYPE_FROM_CLASS (klass),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (ActUserManagerClass, user_is_logged_in_changed),
                              NULL, NULL,
                              g_cclosure_marshal_VOID__OBJECT,
                              G_TYPE_NONE, 1, ACT_TYPE_USER);
        /**
         * ActUserManager::user-changed:
         * @gobject: the object which received the signal
         * @user: the #ActUser that changed
         *
         * One of the users has changed
         */
        signals [USER_CHANGED] =
                g_signal_new ("user-changed",
                              G_TYPE_FROM_CLASS (klass),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (ActUserManagerClass, user_changed),
                              NULL, NULL,
                              g_cclosure_marshal_VOID__OBJECT,
                              G_TYPE_NONE, 1, ACT_TYPE_USER);

        g_type_class_add_private (klass, sizeof (ActUserManagerPrivate));
}

/**
 * act_user_manager_queue_load:
 * @manager: a #ActUserManager
 *
 * Queue loading users into user manager. This must be called, and the
 * #ActUserManager:is-loaded property must be %TRUE before calling
 * act_user_manager_list_users()
 */
static void
act_user_manager_queue_load (ActUserManager *manager)
{
        g_return_if_fail (ACT_IS_USER_MANAGER (manager));

        if (! manager->priv->is_loaded) {
                queue_load_seat_and_users (manager);
        }
}

static gboolean
ensure_accounts_proxy (ActUserManager *manager)
{
        GError *error = NULL;

        if (manager->priv->accounts_proxy != NULL) {
                return TRUE;
        }

        manager->priv->accounts_proxy = accounts_accounts_proxy_new_sync (manager->priv->connection,
                                                                          G_DBUS_PROXY_FLAGS_NONE,
                                                                          ACCOUNTS_NAME,
                                                                          ACCOUNTS_PATH,
                                                                          NULL,
                                                                          &error);
        if (error != NULL) {
                g_debug ("ActUserManager: getting account proxy failed: %s", error->message);
                g_clear_error (&error);
                return FALSE;
        }

        g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (manager->priv->accounts_proxy), G_MAXINT);

        g_object_bind_property (G_OBJECT (manager->priv->accounts_proxy),
                                "has-multiple-users",
                                G_OBJECT (manager),
                                "has-multiple-users",
                                G_BINDING_SYNC_CREATE);

        g_signal_connect (manager->priv->accounts_proxy,
                          "user-added",
                          G_CALLBACK (on_new_user_in_accounts_service),
                          manager);
        g_signal_connect (manager->priv->accounts_proxy,
                          "user-deleted",
                          G_CALLBACK (on_user_removed_in_accounts_service),
                          manager);

        return TRUE;
}

static void
act_user_manager_init (ActUserManager *manager)
{
        GError        *error;

        manager->priv = ACT_USER_MANAGER_GET_PRIVATE (manager);

        act_user_manager_error_quark (); /* register dbus errors */

        /* sessions */
        manager->priv->sessions = g_hash_table_new_full (g_str_hash,
                                                         g_str_equal,
                                                         g_free,
                                                         g_object_unref);

        /* users */
        manager->priv->normal_users_by_name = g_hash_table_new_full (g_str_hash,
                                                                     g_str_equal,
                                                                     g_free,
                                                                     g_object_unref);
        manager->priv->system_users_by_name = g_hash_table_new_full (g_str_hash,
                                                                     g_str_equal,
                                                                     g_free,
                                                                     g_object_unref);
        manager->priv->users_by_object_path = g_hash_table_new_full (g_str_hash,
                                                                     g_str_equal,
                                                                     NULL,
                                                                     g_object_unref);

        error = NULL;
        manager->priv->connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
        if (manager->priv->connection == NULL) {
                if (error != NULL) {
                        g_warning ("Failed to connect to the D-Bus daemon: %s", error->message);
                        g_error_free (error);
                } else {
                        g_warning ("Failed to connect to the D-Bus daemon");
                }
                return;
        }

        ensure_accounts_proxy (manager);

        manager->priv->seat.state = ACT_USER_MANAGER_SEAT_STATE_UNLOADED;
}

static void
act_user_manager_finalize (GObject *object)
{
        ActUserManager *manager;
        GSList         *node;

        g_debug ("ActUserManager: finalizing user manager");

        g_return_if_fail (object != NULL);
        g_return_if_fail (ACT_IS_USER_MANAGER (object));

        manager = ACT_USER_MANAGER (object);

        g_return_if_fail (manager->priv != NULL);

        g_slist_foreach (manager->priv->new_sessions,
                         (GFunc) unload_new_session, NULL);
        g_slist_free (manager->priv->new_sessions);

        g_slist_foreach (manager->priv->fetch_user_requests,
                         (GFunc) free_fetch_user_request, NULL);
        g_slist_free (manager->priv->fetch_user_requests);

        g_slist_free (manager->priv->new_users_inhibiting_load);

        node = manager->priv->new_users;
        while (node != NULL) {
                ActUser *user;
                GSList  *next_node;

                user = ACT_USER (node->data);
                next_node = node->next;

                g_signal_handlers_disconnect_by_func (user, on_new_user_loaded, manager);
                g_object_unref (user);
                manager->priv->new_users = g_slist_delete_link (manager->priv->new_users, node);
                node = next_node;
        }

        unload_seat (manager);

        if (manager->priv->exclude_usernames != NULL) {
                g_slist_foreach (manager->priv->exclude_usernames, (GFunc) g_free, NULL);
                g_slist_free (manager->priv->exclude_usernames);
        }

        if (manager->priv->include_usernames != NULL) {
                g_slist_foreach (manager->priv->include_usernames, (GFunc) g_free, NULL);
                g_slist_free (manager->priv->include_usernames);
        }

        if (manager->priv->seat.seat_proxy != NULL) {
                g_object_unref (manager->priv->seat.seat_proxy);
        }

        if (manager->priv->seat.session_proxy != NULL) {
                g_object_unref (manager->priv->seat.session_proxy);
        }

        if (manager->priv->seat.load_idle_id != 0) {
                g_source_remove (manager->priv->seat.load_idle_id);
        }

#ifdef WITH_SYSTEMD
        if (manager->priv->seat.session_monitor != NULL) {
                sd_login_monitor_unref (manager->priv->seat.session_monitor);
        }

        if (manager->priv->seat.session_monitor_stream != NULL) {
                g_object_unref (manager->priv->seat.session_monitor_stream);
        }

        if (manager->priv->seat.session_monitor_source_id != 0) {
                g_source_remove (manager->priv->seat.session_monitor_source_id);
        }
#endif

        if (manager->priv->accounts_proxy != NULL) {
                g_object_unref (manager->priv->accounts_proxy);
        }

        if (manager->priv->load_id > 0) {
                g_source_remove (manager->priv->load_id);
                manager->priv->load_id = 0;
        }

        g_hash_table_destroy (manager->priv->sessions);

        g_hash_table_destroy (manager->priv->normal_users_by_name);
        g_hash_table_destroy (manager->priv->system_users_by_name);
        g_hash_table_destroy (manager->priv->users_by_object_path);

        G_OBJECT_CLASS (act_user_manager_parent_class)->finalize (object);
}

/**
 * act_user_manager_get_default:
 *
 * Returns the user manager singleton instance.  Calling this function will
 * automatically being loading the user list if it isn't loaded already.
 * The #ActUserManager:is-loaded property will be set to %TRUE when the users
 * are finished loading and then act_user_manager_list_users() can be called.
 *
 * Returns: (transfer none): user manager object
 */
ActUserManager *
act_user_manager_get_default (void)
{
        if (user_manager_object == NULL) {
                user_manager_object = g_object_new (ACT_TYPE_USER_MANAGER, NULL);
                g_object_add_weak_pointer (user_manager_object,
                                           (gpointer *) &user_manager_object);
                act_user_manager_queue_load (user_manager_object);
        }

        return ACT_USER_MANAGER (user_manager_object);
}

/**
 * act_user_manager_no_service:
 * @manager: a #ActUserManager
 *
 * Check whether or not the accounts service is running.
 *
 * Returns: whether or not accounts service is running
 */
gboolean
act_user_manager_no_service (ActUserManager *manager)
{
       return manager->priv->accounts_proxy == NULL;
}

/**
 * act_user_manager_create_user:
 * @manager: a #ActUserManager
 * @username: a unix user name
 * @fullname: a unix GECOS value
 * @accounttype: a #ActUserAccountType
 * @error: a #GError
 *
 * Creates a user account on the system.
 *
 * Returns: (transfer full): user object
 */
ActUser *
act_user_manager_create_user (ActUserManager      *manager,
                              const char          *username,
                              const char          *fullname,
                              ActUserAccountType   accounttype,
                              GError             **error)
{
        GError *local_error = NULL;
        gboolean res;
        gchar *path;
        ActUser *user;

        g_debug ("ActUserManager: Creating user '%s', '%s', %d",
                 username, fullname, accounttype);

        g_assert (manager->priv->accounts_proxy != NULL);

        local_error = NULL;
        res = accounts_accounts_call_create_user_sync (manager->priv->accounts_proxy,
                                                       username,
                                                       fullname,
                                                       accounttype,
                                                       &path,
                                                       NULL,
                                                       &local_error);
        if (! res) {
                g_propagate_error (error, local_error);
                return NULL;
        }

        user = add_new_user_for_object_path (path, manager);

        g_free (path);

        return user;
}

static void
act_user_manager_async_complete_handler (GObject      *source,
                                         GAsyncResult *result,
                                         gpointer      user_data)
{
  GSimpleAsyncResult *res = user_data;

  g_simple_async_result_set_op_res_gpointer (res, g_object_ref (result), g_object_unref);
  g_simple_async_result_complete (res);
  g_object_unref (res);
}

/**
 * act_user_manager_create_user_async:
 * @manager: a #ActUserManager
 * @username: a unix user name
 * @fullname: a unix GECOS value
 * @accounttype: a #ActUserAccountType
 * @cancellable: (allow-none): optional #GCancellable object,
 *     %NULL to ignore
 * @callback: (scope async): a #GAsyncReadyCallback to call
 *     when the request is satisfied
 * @user_data: (closure): the data to pass to @callback
 *
 * Asynchronously creates a user account on the system.
 *
 * For more details, see act_user_manager_create_user(), which
 * is the synchronous version of this call.
 *
 * Since: 0.6.27
 */
void
act_user_manager_create_user_async (ActUserManager      *manager,
                                    const char          *username,
                                    const char          *fullname,
                                    ActUserAccountType   accounttype,
                                    GCancellable        *cancellable,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data)
{
        GSimpleAsyncResult *res;

        g_return_if_fail (ACT_IS_USER_MANAGER (manager));
        g_return_if_fail (manager->priv->accounts_proxy != NULL);

        g_debug ("ActUserManager: Creating user (async) '%s', '%s', %d",
                 username, fullname, accounttype);

        g_assert (manager->priv->accounts_proxy != NULL);

        res = g_simple_async_result_new (G_OBJECT (manager),
                                         callback, user_data,
                                         act_user_manager_create_user_async);
        g_simple_async_result_set_check_cancellable (res, cancellable);

        accounts_accounts_call_create_user (manager->priv->accounts_proxy,
                                            username,
                                            fullname,
                                            accounttype,
                                            cancellable,
                                            act_user_manager_async_complete_handler, res);
}

/**
 * act_user_manager_create_user_finish:
 * @manager: a #ActUserManager
 * @result: a #GAsyncResult
 * @error: a #GError
 *
 * Finishes an asynchronous user creation.
 *
 * See act_user_manager_create_user_async().
 *
 * Returns: (transfer full): user object
 *
 * Since: 0.6.27
 */
ActUser *
act_user_manager_create_user_finish (ActUserManager  *manager,
                                     GAsyncResult    *result,
                                     GError         **error)
{
        GAsyncResult *inner_result;
        ActUser *user = NULL;
        gchar *path;
        GSimpleAsyncResult *res;
        GError *remote_error = NULL;

        g_return_val_if_fail (g_simple_async_result_is_valid (result, G_OBJECT (manager), act_user_manager_create_user_async), FALSE);

        res = G_SIMPLE_ASYNC_RESULT (result);
        inner_result = g_simple_async_result_get_op_res_gpointer (res);
        g_assert (inner_result);

        if (accounts_accounts_call_create_user_finish (manager->priv->accounts_proxy,
                                                       &path, inner_result, &remote_error)) {
                user = add_new_user_for_object_path (path, manager);
                g_free (path);
        }

        if (remote_error) {
                g_dbus_error_strip_remote_error (remote_error);
                g_propagate_error (error, remote_error);
        }

        return user;
}

/**
 * act_user_manager_cache_user:
 * @manager: a #ActUserManager
 * @username: a user name
 * @error: a #GError
 *
 * Caches a user account so it shows up via act_user_manager_list_users().
 *
 * Returns: (transfer full): user object
 */
ActUser *
act_user_manager_cache_user (ActUserManager     *manager,
                             const char         *username,
                             GError            **error)
{
        GError *local_error = NULL;
        gboolean res;
        gchar *path;
        ActUser *user;

        g_debug ("ActUserManager: Caching user '%s'",
                 username);

        g_assert (manager->priv->accounts_proxy != NULL);

        local_error = NULL;
        res = accounts_accounts_call_cache_user_sync (manager->priv->accounts_proxy,
                                                      username,
                                                      &path,
                                                      NULL,
                                                      &local_error);
        if (! res) {
                g_propagate_error (error, local_error);
                return NULL;
        }

        user = add_new_user_for_object_path (path, manager);

        g_free (path);

        return user;
}


/**
 * act_user_manager_cache_user_async:
 * @manager: a #ActUserManager
 * @username: a unix user name
 * @cancellable: (allow-none): optional #GCancellable object,
 *     %NULL to ignore
 * @callback: (scope async): a #GAsyncReadyCallback to call
 *     when the request is satisfied
 * @user_data: (closure): the data to pass to @callback
 *
 * Asynchronously caches a user account so it shows up via
 * act_user_manager_list_users().
 *
 * For more details, see act_user_manager_cache_user(), which
 * is the synchronous version of this call.
 *
 * Since: 0.6.27
 */
void
act_user_manager_cache_user_async (ActUserManager      *manager,
                                   const char          *username,
                                   GCancellable        *cancellable,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data)
{
        GSimpleAsyncResult *res;

        g_return_if_fail (ACT_IS_USER_MANAGER (manager));
        g_return_if_fail (manager->priv->accounts_proxy != NULL);

        g_debug ("ActUserManager: Caching user (async) '%s'", username);

        res = g_simple_async_result_new (G_OBJECT (manager),
                                         callback, user_data,
                                         act_user_manager_cache_user_async);
        g_simple_async_result_set_check_cancellable (res, cancellable);

        accounts_accounts_call_cache_user (manager->priv->accounts_proxy,
                                           username,
                                           cancellable,
                                           act_user_manager_async_complete_handler, res);
}

/**
 * act_user_manager_cache_user_finish:
 * @manager: a #ActUserManager
 * @result: a #GAsyncResult
 * @error: a #GError
 *
 * Finishes an asynchronous user caching.
 *
 * See act_user_manager_cache_user_async().
 *
 * Returns: (transfer full): user object
 *
 * Since: 0.6.27
 */
ActUser *
act_user_manager_cache_user_finish (ActUserManager  *manager,
                                    GAsyncResult    *result,
                                    GError         **error)
{
        GAsyncResult *inner_result;
        ActUser *user = NULL;
        gchar *path;
        GSimpleAsyncResult *res;
        GError *remote_error = NULL;

        g_return_val_if_fail (g_simple_async_result_is_valid (result, G_OBJECT (manager), act_user_manager_cache_user_async), FALSE);

        res = G_SIMPLE_ASYNC_RESULT (result);
        inner_result = g_simple_async_result_get_op_res_gpointer (res);
        g_assert (inner_result);

        if (accounts_accounts_call_cache_user_finish (manager->priv->accounts_proxy,
                                                      &path, inner_result, &remote_error)) {
                user = add_new_user_for_object_path (path, manager);
                g_free (path);
        }

        if (remote_error) {
                g_dbus_error_strip_remote_error (remote_error);
                g_propagate_error (error, remote_error);
        }

        return user;
}

/**
 * act_user_manager_uncache_user:
 * @manager: a #ActUserManager
 * @username: a user name
 * @error: a #GError
 *
 * Releases all metadata about a user account, including icon,
 * language and session. If the user account is from a remote
 * server and the user has never logged in before, then that
 * account will no longer show up in ListCachedUsers() output.
 *
 * Returns: %TRUE if successful, otherwise %FALSE
 */
gboolean
act_user_manager_uncache_user (ActUserManager     *manager,
                               const char         *username,
                               GError            **error)
{
        GError *local_error = NULL;
        gboolean res;

        g_debug ("ActUserManager: Uncaching user '%s'",
                 username);

        g_assert (manager->priv->accounts_proxy != NULL);

        local_error = NULL;
        res = accounts_accounts_call_uncache_user_sync (manager->priv->accounts_proxy,
                                                        username,
                                                        NULL,
                                                        &local_error);
        if (! res) {
                g_propagate_error (error, local_error);
                return FALSE;
        }

        return TRUE;
}

/*
 * act_user_manager_uncache_user_async:
 * @manager: a #ActUserManager
 * @username: a unix user name
 * @cancellable: (allow-none): optional #GCancellable object,
 *     %NULL to ignore
 * @callback: (scope async): a #GAsyncReadyCallback to call
 *     when the request is satisfied
 * @user_data: (closure): the data to pass to @callback
 *
 * Asynchronously uncaches a user account.
 *
 * For more details, see act_user_manager_uncache_user(), which
 * is the synchronous version of this call.
 *
 * Since: 0.6.39
 */
void
act_user_manager_uncache_user_async (ActUserManager      *manager,
                                     const char          *username,
                                     GCancellable        *cancellable,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data)
{
        GSimpleAsyncResult *res;

        g_return_if_fail (ACT_IS_USER_MANAGER (manager));
        g_return_if_fail (username != NULL);
        g_return_if_fail (manager->priv->accounts_proxy != NULL);

        g_debug ("ActUserManager: Uncaching user (async) '%s'", username);

        res = g_simple_async_result_new (G_OBJECT (manager),
                                         callback, user_data,
                                         act_user_manager_uncache_user_async);
        g_simple_async_result_set_check_cancellable (res, cancellable);

        accounts_accounts_call_uncache_user (manager->priv->accounts_proxy,
                                             username,
                                             cancellable,
                                             act_user_manager_async_complete_handler, res);
}

/**
 * act_user_manager_uncache_user_finish:
 * @manager: a #ActUserManager
 * @result: a #GAsyncResult
 * @error: a #GError
 *
 * Finishes an asynchronous user uncaching.
 *
 * See act_user_manager_uncache_user_async().
 *
 * Returns: %TRUE if the user account was successfully uncached
 *
 * Since: 0.6.39
 */
gboolean
act_user_manager_uncache_user_finish (ActUserManager  *manager,
                                      GAsyncResult    *result,
                                      GError         **error)
{
        GAsyncResult *inner_result;
        gboolean success;
        GSimpleAsyncResult *res;
        GError *remote_error = NULL;

        g_return_val_if_fail (g_simple_async_result_is_valid (result, G_OBJECT (manager), act_user_manager_uncache_user_async), FALSE);

        res = G_SIMPLE_ASYNC_RESULT (result);
        inner_result = g_simple_async_result_get_op_res_gpointer (res);
        g_assert (inner_result);

        success = accounts_accounts_call_uncache_user_finish (manager->priv->accounts_proxy,
                                                              inner_result, &remote_error);

        if (remote_error) {
                g_dbus_error_strip_remote_error (remote_error);
                g_propagate_error (error, remote_error);
        }

        return success;
}

/**
 * act_user_manager_delete_user:
 * @manager: a #ActUserManager
 * @user: an #ActUser object
 * @remove_files: %TRUE to delete the users home directory
 * @error: a #GError
 *
 * Deletes a user account on the system.
 *
 * Returns: %TRUE if the user account was successfully deleted
 */
gboolean
act_user_manager_delete_user (ActUserManager  *manager,
                              ActUser         *user,
                              gboolean         remove_files,
                              GError         **error)
{
        GError *local_error;
        gboolean res = TRUE;

        g_debug ("ActUserManager: Deleting user '%s' (uid %ld)", act_user_get_user_name (user), (long) act_user_get_uid (user));

        g_return_val_if_fail (ACT_IS_USER_MANAGER (manager), FALSE);
        g_return_val_if_fail (ACT_IS_USER (user), FALSE);
        g_return_val_if_fail (manager->priv->accounts_proxy != NULL, FALSE);

        local_error = NULL;
        if (!accounts_accounts_call_delete_user_sync (manager->priv->accounts_proxy,
                                                      act_user_get_uid (user),
                                                      remove_files,
                                                      NULL,
                                                      &local_error)) {
                g_propagate_error (error, local_error);
                res = FALSE;
        }

        return res;
}

/**
 * act_user_manager_delete_user_async:
 * @manager: a #ActUserManager
 * @user: a #ActUser object
 * @remove_files: %TRUE to delete the users home directory
 * @cancellable: (allow-none): optional #GCancellable object,
 *     %NULL to ignore
 * @callback: (scope async): a #GAsyncReadyCallback to call
 *     when the request is satisfied
 * @user_data: (closure): the data to pass to @callback
 *
 * Asynchronously deletes a user account from the system.
 *
 * For more details, see act_user_manager_delete_user(), which
 * is the synchronous version of this call.
 *
 * Since: 0.6.27
 */
void
act_user_manager_delete_user_async (ActUserManager      *manager,
                                    ActUser             *user,
                                    gboolean             remove_files,
                                    GCancellable        *cancellable,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data)
{
        GSimpleAsyncResult *res;

        g_return_if_fail (ACT_IS_USER_MANAGER (manager));
        g_return_if_fail (ACT_IS_USER (user));
        g_return_if_fail (manager->priv->accounts_proxy != NULL);

        res = g_simple_async_result_new (G_OBJECT (manager),
                                         callback, user_data,
                                         act_user_manager_delete_user_async);
        g_simple_async_result_set_check_cancellable (res, cancellable);

        g_debug ("ActUserManager: Deleting (async) user '%s' (uid %ld)", act_user_get_user_name (user), (long) act_user_get_uid (user));

        accounts_accounts_call_delete_user (manager->priv->accounts_proxy,
                                            act_user_get_uid (user), remove_files,
                                            cancellable,
                                            act_user_manager_async_complete_handler, res);
}

/**
 * act_user_manager_delete_user_finish:
 * @manager: a #ActUserManager
 * @result: a #GAsyncResult
 * @error: a #GError
 *
 * Finishes an asynchronous user account deletion.
 *
 * See act_user_manager_delete_user_async().
 *
 * Returns: %TRUE if the user account was successfully deleted
 *
 * Since: 0.6.27
 */
gboolean
act_user_manager_delete_user_finish (ActUserManager  *manager,
                                     GAsyncResult    *result,
                                     GError         **error)
{
        GAsyncResult *inner_result;
        gboolean success;
        GSimpleAsyncResult *res;
        GError *remote_error = NULL;

        g_return_val_if_fail (g_simple_async_result_is_valid (result, G_OBJECT (manager), act_user_manager_delete_user_async), FALSE);
        res = G_SIMPLE_ASYNC_RESULT (result);
        inner_result = g_simple_async_result_get_op_res_gpointer (res);
        g_assert (inner_result);

        success = accounts_accounts_call_delete_user_finish (manager->priv->accounts_proxy,
                                                             inner_result, &remote_error);
        if (remote_error) {
                g_dbus_error_strip_remote_error (remote_error);
                g_propagate_error (error, remote_error);
        }

        return success;
}
