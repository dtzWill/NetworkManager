/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager system settings service - keyfile plugin
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
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2018 Red Hat, Inc.
 */

#include "nm-default.h"

#include "nms-keyfile-storage.h"

#include "nms-keyfile-plugin.h"

/*****************************************************************************/

struct _NMSKeyfileStorageClass {
	NMSettingsStorageClass parent;
};

G_DEFINE_TYPE (NMSKeyfileStorage, nms_keyfile_storage, NM_TYPE_SETTINGS_STORAGE)

static void
nms_keyfile_storage_init (NMSKeyfileStorage *self)
{
}

NMSKeyfileStorage *
nms_keyfile_storage_new (NMSKeyfilePlugin *plugin)
{
	nm_assert (NMS_IS_KEYFILE_PLUGIN (plugin));

	return g_object_new (NMS_TYPE_KEYFILE_STORAGE,
	                     NM_SETTINGS_STORAGE_PLUGIN, plugin,
	                     NULL);
}

static void
nms_keyfile_storage_class_init (NMSKeyfileStorageClass *klass)
{
}
