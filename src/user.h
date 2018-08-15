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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef __USER__
#define __USER__

#include <sys/types.h>
#include <pwd.h>
#include <shadow.h>

#include <glib.h>
#include <gio/gio.h>

#include "types.h"

G_BEGIN_DECLS

#define TYPE_USER (user_get_type ())
#define USER(object) (G_TYPE_CHECK_INSTANCE_CAST ((object), TYPE_USER, User))
#define IS_USER(object) (G_TYPE_CHECK_INSTANCE_TYPE ((object), TYPE_USER))

typedef enum {
        ACCOUNT_TYPE_STANDARD,
        ACCOUNT_TYPE_ADMINISTRATOR,
#define ACCOUNT_TYPE_LAST ACCOUNT_TYPE_ADMINISTRATOR
} AccountType;

typedef enum {
        PASSWORD_MODE_REGULAR,
        PASSWORD_MODE_SET_AT_LOGIN,
        PASSWORD_MODE_NONE,
#define PASSWORD_MODE_LAST PASSWORD_MODE_NONE
} PasswordMode;

/* local methods */

GType          user_get_type                (void) G_GNUC_CONST;
User *         user_new                     (Daemon        *daemon,
                                             uid_t          uid);

void           user_update_from_pwent       (User          *user,
                                             struct passwd *pwent,
                                             struct spwd   *spent);
void           user_update_from_keyfile     (User          *user,
                                             GKeyFile      *keyfile);
void           user_update_local_account_property (User          *user,
                                                   gboolean       local);
void           user_update_system_account_property (User          *user,
                                                    gboolean       system);
gboolean       user_get_cached              (User          *user);
void           user_set_cached              (User          *user,
                                             gboolean       cached);
void           user_set_saved               (User          *user,
                                             gboolean       saved);

void           user_register                (User          *user);
void           user_unregister              (User          *user);
void           user_changed                 (User          *user);

void           user_save                    (User          *user);

const gchar *  user_get_user_name           (User          *user);
gboolean       user_get_system_account      (User          *user);
gboolean       user_get_local_account       (User          *user);
const gchar *  user_get_object_path         (User          *user);
uid_t          user_get_uid                 (User          *user);
const gchar *  user_get_shell               (User          *user);

G_END_DECLS

#endif
