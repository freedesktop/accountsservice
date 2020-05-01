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

#ifndef __UTIL_H__
#define __UTIL_H__

#include <glib.h>

G_BEGIN_DECLS

void sys_log (GDBusMethodInvocation *context,
              const gchar           *format,
                                     ...);

gboolean get_caller_uid (GDBusMethodInvocation *context, gint *uid);

gboolean spawn_with_login_uid (GDBusMethodInvocation  *context,
                               const gchar            *argv[],
                               GError                **error);

gboolean get_admin_groups (gid_t  *admin_gid_out,
                           gid_t **groups_out,
                           gsize  *n_groups_out);

gint get_user_groups (const gchar  *username,
                      gid_t         group,
                      gid_t       **groups);

G_END_DECLS

#endif /* __UTIL_H__ */
