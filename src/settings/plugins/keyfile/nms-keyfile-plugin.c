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
 * Copyright (C) 2008 Novell, Inc.
 * Copyright (C) 2008 - 2018 Red Hat, Inc.
 */

#include "nm-default.h"

#include "nms-keyfile-plugin.h"

#include <sys/stat.h>
#include <unistd.h>
#include <sys/types.h>
#include <string.h>
#include <glib/gstdio.h>

#include "nm-utils/c-list-util.h"
#include "nm-utils/nm-io-utils.h"

#include "nm-connection.h"
#include "nm-setting.h"
#include "nm-setting-connection.h"
#include "nm-utils.h"
#include "nm-config.h"
#include "nm-core-internal.h"
#include "nm-keyfile-internal.h"

#include "systemd/nm-sd-utils.h"

#include "settings/nm-settings-plugin.h"

#include "nms-keyfile-storage.h"
#include "nms-keyfile-writer.h"
#include "nms-keyfile-reader.h"
#include "nms-keyfile-utils.h"

/*****************************************************************************/

typedef struct {
	char *uuid;
	NMSKeyfileStorage *storage;
} EventsDelData;

typedef struct {
	CList lst_head;
	GHashTable *idx;

	// XXX: do we need filename-idx?
	GHashTable *filename_idx;
} IdxCollection;

typedef struct {
	CList cisd_lst;
	char *full_filename;
	const char *filename;

	/* the profile loaded from the file. Note that this profile is only relevant
	 * during _do_reload_all(). The winning profile at the end of reload will
	 * be referenced as connection_exported, the connection field here will be
	 * cleared. */
	NMConnection *connection;

	/* the following fields are only required during _do_reload_all() for comparing
	 * which profile is the most relevant one (in case multple files provide a profile
	 * with the same UUID). */
	struct timespec stat_mtime;
	dev_t stat_dev;
	ino_t stat_ino;
	NMSKeyfileStorageType storage_type:3;
	guint storage_priority:15;
} ConnInfoStorageData;

typedef struct _NMSKeyfileConnInfo {
	/* this must be the first element, because we use it for hashing with
	 * nm_pstr_*(). It points to the end of the structure (uuid_buf). */
	const char *uuid;

	CList conn_info_lst;

	NMSKeyfileStorage *storage;

	/* the list of files associated with this UUID. In general, any number of
	 * files can connection profiles for a particular UUID. During _do_reload_all(),
	 * we need to get a list of all of them an keep the best one.
	 *
	 * However, also aside _do_reload_all() we keep this list of the files to know
	 * which files referenced this UUID at the time of loading. */
	CList cisd_lst_head;

	NMSKeyfileStorageType storage_type_exported;

	NMConnection       *connection_exported;

	/* these field are only used during _do_reload_all() while building
	 * the list of files. */
	char *_loaded_path_etc;
	char *_loaded_path_run;

	char uuid_buf[];
} NMSKeyfileConnInfo;

typedef struct {

	/* there can/could be multiple read-only directories. For example, one
	 * could set dirname_libs to
	 *   - /usr/lib/NetworkManager/profiles/
	 *   - /etc/NetworkManager/system-connections
	 * and leave dirname_etc unset. In this case, there would be multiple
	 * read-only directories.
	 *
	 * Directories that come later have higher priority and shadow profiles
	 * from earlier directories.
	 */
	char **dirname_libs;
	char *dirname_etc;
	char *dirname_run;

	IdxCollection conn_infos;

	NMConfig *config;

	GFileMonitor **monitors;

	bool initialized;
} NMSKeyfilePluginPrivate;

struct _NMSKeyfilePlugin {
	NMSettingsPlugin parent;
	NMSKeyfilePluginPrivate _priv;
};

struct _NMSKeyfilePluginClass {
	NMSettingsPluginClass parent;
};

G_DEFINE_TYPE (NMSKeyfilePlugin, nms_keyfile_plugin, NM_TYPE_SETTINGS_PLUGIN)

#define NMS_KEYFILE_PLUGIN_GET_PRIVATE(self) _NM_GET_PRIVATE (self, NMSKeyfilePlugin, NMS_IS_KEYFILE_PLUGIN, NMSettingsPlugin)

/*****************************************************************************/

#define _NMLOG_PREFIX_NAME      "keyfile"
#define _NMLOG_DOMAIN           LOGD_SETTINGS
#define _NMLOG(level, ...) \
    nm_log ((level), _NMLOG_DOMAIN, NULL, NULL, \
            "%s" _NM_UTILS_MACRO_FIRST (__VA_ARGS__), \
            _NMLOG_PREFIX_NAME": " \
            _NM_UTILS_MACRO_REST (__VA_ARGS__))

/*****************************************************************************/

static gboolean
_ignore_filename (NMSKeyfileStorageType storage_type,
                  const char *filename)
{
	/* for backward-compatibility, we don't require an extension for
	 * files under "/etc/...". */
	return nm_keyfile_utils_ignore_filename (filename,
	                                         (storage_type != NMS_KEYFILE_STORAGE_TYPE_ETC));
}

/*****************************************************************************/

static const char *
_get_plugin_dir (NMSKeyfilePluginPrivate *priv)
{
	/* the plugin dir is only needed to generate connection.uuid value via
	 * nm_keyfile_read_ensure_uuid(). This is either the configured /etc
	 * directory, of the compile-time default (in case the /etc directory
	 * is disabled). */
	return priv->dirname_etc ?: NM_KEYFILE_PATH_NAME_ETC_DEFAULT;
}

static gboolean
_path_detect_storage_type (const char *full_filename,
                           const char *const*dirname_libs,
                           const char *dirname_etc,
                           const char *dirname_run,
                           NMSKeyfileStorageType *out_storage_type,
                           const char **out_dirname,
                           const char **out_filename,
                           GError **error)
{
	NMSKeyfileStorageType storage_type;
	const char *filename = NULL;
	const char *dirname = NULL;
	guint i;

	if (full_filename[0] != '/') {
		nm_utils_error_set_literal (error, NM_UTILS_ERROR_UNKNOWN,
		                            "filename is not an absolute path");
		return FALSE;
	}

	if (   dirname_run
	    && (filename = nm_utils_file_is_in_path (full_filename, dirname_run))) {
		storage_type = NMS_KEYFILE_STORAGE_TYPE_RUN;
		dirname = dirname_run;
	} else if (   dirname_etc
	           && (filename = nm_utils_file_is_in_path (full_filename, dirname_etc))) {
		storage_type = NMS_KEYFILE_STORAGE_TYPE_ETC;
		dirname = dirname_etc;
	} else {
		for (i = 0; dirname_libs && dirname_libs[i]; i++) {
			if ((filename = nm_utils_file_is_in_path (full_filename, dirname_libs[i]))) {
				storage_type = NMS_KEYFILE_STORAGE_TYPE_LIB;
				dirname = dirname_libs[i];
				break;
			}
		}
		if (!dirname) {
			nm_utils_error_set_literal (error, NM_UTILS_ERROR_UNKNOWN,
			                            "filename is not inside a keyfile directory");
			return FALSE;
		}
	}

	if (_ignore_filename (storage_type, filename)) {
		nm_utils_error_set_literal (error, NM_UTILS_ERROR_UNKNOWN,
		                            "filename is not a valid keyfile");
		return FALSE;
	}

	NM_SET_OUT (out_storage_type, storage_type);
	NM_SET_OUT (out_dirname, dirname);
	NM_SET_OUT (out_filename, filename);
	return TRUE;
}

/*****************************************************************************/

static NMConnection *
_read_from_file (const char *full_filename,
                 const char *plugin_dir,
                 struct stat *out_stat,
                 GError **error)
{
	NMConnection *connection;

	g_return_val_if_fail (full_filename && full_filename[0] == '/', NULL);

	connection = nms_keyfile_reader_from_file (full_filename, plugin_dir, out_stat, error);
	if (!connection)
		return NULL;

	nm_assert (nm_connection_verify (connection, NULL));
	nm_assert (nm_connection_get_uuid (connection));
	return connection;
}

/*****************************************************************************/

static void
_conn_info_storage_data_destroy (ConnInfoStorageData *storage_data)
{
	c_list_unlink_stale (&storage_data->cisd_lst);
	g_free (storage_data->full_filename);
	nm_g_object_unref (storage_data->connection);
	g_slice_free (ConnInfoStorageData, storage_data);
}

static ConnInfoStorageData *
_conn_info_storage_data_new (guint storage_priority,
                             NMSKeyfileStorageType storage_type,
                             char *full_filename_take,
                             NMConnection *connection_take,
                             const struct stat *st)
{
	ConnInfoStorageData *storage_data;

	storage_data = g_slice_new0 (ConnInfoStorageData);
	storage_data->storage_type = storage_type;
	storage_data->storage_priority = storage_priority;
	storage_data->full_filename = full_filename_take;
	storage_data->filename = strrchr (full_filename_take, '/') + 1;
	storage_data->connection = connection_take;
	if (st) {
		storage_data->stat_mtime = st->st_mtim;
		storage_data->stat_dev = st->st_dev;
		storage_data->stat_ino = st->st_ino;
	}

	nm_assert (storage_data->storage_type     == storage_type);
	nm_assert (storage_data->storage_priority == storage_priority);
	nm_assert (storage_data->full_filename);
	nm_assert (storage_data->full_filename[0] == '/');
	nm_assert (storage_data->filename);
	nm_assert (storage_data->filename[0]);
	nm_assert (!strchr (storage_data->filename, '/'));

	return storage_data;
}

static void
_conn_info_storage_data_destroy_all (CList *cisd_lst_head)
{
	ConnInfoStorageData *storage_data;

	while ((storage_data = c_list_first_entry (cisd_lst_head, ConnInfoStorageData, cisd_lst)))
		_conn_info_storage_data_destroy (storage_data);
}

static int
_conn_info_storage_data_cmp (const CList *lst_a,
                             const CList *lst_b,
                             const void *user_data)
{
	const ConnInfoStorageData *a = c_list_entry (lst_a, ConnInfoStorageData, cisd_lst);
	const ConnInfoStorageData *b = c_list_entry (lst_b, ConnInfoStorageData, cisd_lst);

	/* we sort more important entries first. */

	/* sorting by storage-priority implies sorting by storage-type too.
	 * That is, because for different storage-types, we assign different storage-priorities
	 * and their sort order corresponds (with inverted order). Assert for that. */
	nm_assert (   a->storage_type == b->storage_type
	           || (   (a->storage_priority != b->storage_priority)
	               && (a->storage_type < b->storage_type) == (a->storage_priority > b->storage_priority)));

	/* sort by storage-priority, smaller is more important. */
	NM_CMP_FIELD_UNSAFE (a, b, storage_priority);

	/* newer files are more important. */
	NM_CMP_FIELD (b, a, stat_mtime.tv_sec);
	NM_CMP_FIELD (b, a, stat_mtime.tv_nsec);

	NM_CMP_FIELD_STR (a, b, filename);

	nm_assert_not_reached ();
	return 0;
}

static gboolean
_conn_info_storage_data_prioritize_loaded (CList *cisd_lst_head,
                                           const char *loaded_path)
{
	ConnInfoStorageData *storage_data;
	struct stat st_loaded;

	if (loaded_path[0] != '/')
		return FALSE;

	/* we compare the file based on the inode, not based on the path.
	 * stat() the file. */
	if (stat (loaded_path, &st_loaded) != 0)
		return FALSE;

	c_list_for_each_entry (storage_data, cisd_lst_head, cisd_lst) {
		if (   storage_data->stat_dev != st_loaded.st_dev
		    || storage_data->stat_ino != st_loaded.st_ino)
			continue;

		if (cisd_lst_head->next != &storage_data->cisd_lst) {
			/* move to front. */
			c_list_unlink_stale (&storage_data->cisd_lst);
			c_list_link_front (cisd_lst_head, &storage_data->cisd_lst);
		}
		return TRUE;
	}

	return FALSE;
}

/*****************************************************************************/

static NMSKeyfileConnInfo *
_conn_info_new (const char *uuid)
{
	NMSKeyfileConnInfo *conn_info;
	gsize uuid_len;

	nm_assert (nm_utils_is_uuid (uuid));

	uuid_len = strlen (uuid);

	conn_info = g_malloc0 (sizeof (NMSKeyfileConnInfo) + 1 + uuid_len);
	conn_info->uuid = conn_info->uuid_buf;
	memcpy (conn_info->uuid_buf, uuid, uuid_len + 1);
	c_list_init (&conn_info->conn_info_lst);
	c_list_init (&conn_info->cisd_lst_head);
	return conn_info;
}

static void
_conn_info_destroy (NMSKeyfileConnInfo *conn_info)
{
	nm_assert (conn_info);
	nm_assert (!conn_info->storage || conn_info->storage->conn_info == conn_info);

	_conn_info_storage_data_destroy_all (&conn_info->cisd_lst_head);

	c_list_unlink_stale (&conn_info->conn_info_lst);

	if (conn_info->storage) {
		conn_info->storage->conn_info = NULL;
		g_object_unref (conn_info->storage);
	}

	nm_g_object_unref (conn_info->connection_exported);

	g_free (conn_info->_loaded_path_run);
	g_free (conn_info->_loaded_path_etc);

	g_free (conn_info);
}

static NMSKeyfileConnInfo *
_conn_info_from_storage (gpointer plugin  /* NMSKeyfilePlugin  */,
                         gpointer storage /* NMSKeyfileStorage */,
                         GError **error)
{
	NMSKeyfileConnInfo *conn_info;

	if (   NMS_IS_KEYFILE_STORAGE (storage)
	    && (conn_info = NMS_KEYFILE_STORAGE (storage)->conn_info)
	    && NMS_IS_KEYFILE_PLUGIN (plugin)
	    && plugin == nm_settings_storage_get_plugin (storage))
		return conn_info;

	nm_utils_error_set_literal (error, NM_UTILS_ERROR_UNKNOWN,
	                            "Missing storage for keyfile");
	return NULL;
}

static void
_conn_info_ensure_storage (NMSKeyfilePlugin *self,
                           NMSKeyfileConnInfo *conn_info)
{
	if (   !conn_info->storage
	    && conn_info->connection_exported) {
		conn_info->storage = nms_keyfile_storage_new (self);
		conn_info->storage->conn_info = conn_info;
	}
}

static gboolean
_conn_info_has_equal_connection (NMSKeyfileConnInfo *conn_info,
                                 NMConnection *connection)
{
	nm_assert (NM_IS_CONNECTION (connection));

	return    conn_info->connection_exported
	       && nm_connection_compare (connection,
	                                 conn_info->connection_exported,
	                                   NM_SETTING_COMPARE_FLAG_IGNORE_AGENT_OWNED_SECRETS
	                                 | NM_SETTING_COMPARE_FLAG_IGNORE_NOT_SAVED_SECRETS);
}

/*****************************************************************************/

static NMSKeyfileConnInfo *
_conn_infos_get (const IdxCollection *conn_infos,
                 const char *uuid)
{
	return g_hash_table_lookup (conn_infos->idx, &uuid);
}

static NMSKeyfileConnInfo *
_conn_infos_add (IdxCollection *conn_infos,
                 const char *uuid)
{
	NMSKeyfileConnInfo *conn_info;

	conn_info = _conn_infos_get (conn_infos, uuid);
	if (!conn_info) {
		conn_info = _conn_info_new (uuid);
		g_hash_table_add (conn_infos->idx, conn_info);
		c_list_link_tail (&conn_infos->lst_head, &conn_info->conn_info_lst);
	}

	return conn_info;
}

static void
_conn_infos_register_filename (IdxCollection *conn_infos,
                               NMSKeyfileConnInfo *conn_info,
                               const char *dirname,
                               const char *filename)
{
	gs_free char *full_filename = NULL;
	ConnInfoStorageData *sd;

	full_filename = g_build_filename (dirname, filename, NULL);

	c_list_for_each_entry (sd, &conn_info->cisd_lst_head, cisd_lst) {
		if (nm_streq (sd->full_filename, full_filename)) {
			/* we already have it registered. Move the entry to the beginning of
			 * the list. */
			c_list_unlink_stale (&sd->cisd_lst);
			c_list_link_front (&conn_info->cisd_lst_head, &sd->cisd_lst);
			return;
		}
	}

	/* prepend the filename to the front and index it. The priority and stat
	 * only matter during reload, so we don't need it in this case (we are
	 * called during a regular load of one file). */
	sd = _conn_info_storage_data_new (0,
	                                  NMS_KEYFILE_STORAGE_TYPE_RUN,
	                                  g_steal_pointer (&full_filename),
	                                  NULL,
	                                  NULL);
	c_list_link_front (&conn_info->cisd_lst_head, &sd->cisd_lst);
	g_hash_table_insert (conn_infos->filename_idx,
	                     sd->full_filename,
	                     conn_info);
}

static void
_conn_infos_remove (IdxCollection *conn_infos,
                    NMSKeyfileConnInfo *conn_info)
{
	nm_assert (conn_infos);
	nm_assert (conn_info);
	nm_assert (c_list_contains (&conn_infos->lst_head, &conn_info->conn_info_lst));
	nm_assert (g_hash_table_contains (conn_infos->idx, conn_info));

	g_hash_table_remove (conn_infos->idx, conn_info);
}

/*****************************************************************************/

static void
_load_dir (IdxCollection *conn_infos,
           guint storage_priority,
           NMSKeyfileStorageType storage_type,
           const char *dirname,
           const char *plugin_dir)
{
	const char *filename;
	GDir *dir;

	if (!dirname)
		return;

	dir = g_dir_open (dirname, 0, NULL);
	if (!dir)
		return;

	while ((filename = g_dir_read_name (dir))) {
		gs_unref_object NMConnection *connection = NULL;
		gs_free_error GError *error = NULL;
		NMSKeyfileConnInfo *conn_info;
		ConnInfoStorageData *storage_data;
		gs_free char *full_filename = NULL;
		struct stat st;

		if (_ignore_filename (storage_type, filename)) {
			gs_free char *loaded_uuid = NULL;
			gs_free char *loaded_path = NULL;

			if (!nms_keyfile_loaded_uuid_read (dirname,
			                                   filename,
			                                   NULL,
			                                   &loaded_uuid,
			                                   &loaded_path)) {
				_LOGT ("load: \"%s/%s\": skip file due to filename pattern", dirname, filename);
				continue;
			}
			if (!NM_IN_SET (storage_type, NMS_KEYFILE_STORAGE_TYPE_RUN,
			                              NMS_KEYFILE_STORAGE_TYPE_ETC)) {
				_LOGT ("load: \"%s/%s\": skip loaded file from read-only directory", dirname, filename);
				continue;
			}
			conn_info = _conn_infos_add (conn_infos, loaded_uuid);
			if (storage_type == NMS_KEYFILE_STORAGE_TYPE_RUN) {
				nm_assert (!conn_info->_loaded_path_run);
				conn_info->_loaded_path_run = g_steal_pointer (&loaded_path);
			} else {
				nm_assert (!conn_info->_loaded_path_etc);
				conn_info->_loaded_path_etc = g_steal_pointer (&loaded_path);
			}
			continue;
		}

		full_filename = g_build_filename (dirname, filename, NULL);

		connection = _read_from_file (full_filename, plugin_dir, &st, &error);
		if (!connection) {
			_LOGW ("load: \"%s\": failed to load connection: %s", full_filename, error->message);
			continue;
		}

		conn_info = _conn_infos_add (conn_infos, nm_connection_get_uuid (connection));
		storage_data = _conn_info_storage_data_new (storage_priority,
		                                            storage_type,
		                                            g_steal_pointer (&full_filename),
		                                            g_steal_pointer (&connection),
		                                            &st);
		c_list_link_tail (&conn_info->cisd_lst_head, &storage_data->cisd_lst);
	}

	g_dir_close (dir);
}

static void
_do_reload_all (NMSKeyfilePlugin *self)
{
	NMSKeyfilePluginPrivate *priv = NMS_KEYFILE_PLUGIN_GET_PRIVATE (self);
	NMSKeyfileConnInfo *conn_info, *conn_info_safe;
	gs_unref_ptrarray GPtrArray *events_mod = NULL;
	gs_unref_array    GArray    *events_del = NULL;
	guint i;
	const char *plugin_dir = _get_plugin_dir (priv);

	priv->initialized = TRUE;

	/* while reloading, we let the filename index degrade. */
	g_hash_table_remove_all (priv->conn_infos.filename_idx);

	c_list_for_each_entry (conn_info, &priv->conn_infos.lst_head, conn_info_lst)
		_conn_info_storage_data_destroy_all (&conn_info->cisd_lst_head);

	/* collect all information by loading it from the files. On repeated reloads,
	 * this merges new information with content from previous loads. */
	_load_dir (&priv->conn_infos, 1, NMS_KEYFILE_STORAGE_TYPE_RUN, priv->dirname_run, plugin_dir);
	_load_dir (&priv->conn_infos, 2, NMS_KEYFILE_STORAGE_TYPE_ETC, priv->dirname_etc, plugin_dir);
	for (i = 0; priv->dirname_libs && priv->dirname_libs[i]; i++)
		_load_dir (&priv->conn_infos, 3 + i, NMS_KEYFILE_STORAGE_TYPE_LIB, priv->dirname_libs[i], plugin_dir);

	/* sort out the loaded information. */
	c_list_for_each_entry_safe (conn_info, conn_info_safe, &priv->conn_infos.lst_head, conn_info_lst) {
		ConnInfoStorageData *sd, *sd_best;
		gboolean connection_modified;
		gboolean loaded_path_masked = FALSE;
		const char *loaded_dirname = NULL;
		gs_free char *loaded_path = NULL;

		/* find and steal the loaded-path (if any) */
		if (conn_info->_loaded_path_run) {
			if (conn_info->_loaded_path_etc) {
				gs_free char *f1 = NULL;
				gs_free char *f2 = NULL;

				_LOGT ("load: \"%s\": shadowed by \"%s\"",
				       (f1 = nms_keyfile_loaded_uuid_filename (priv->dirname_etc, conn_info->uuid, FALSE)),
				       (f2 = nms_keyfile_loaded_uuid_filename (priv->dirname_run, conn_info->uuid, FALSE)));
				nm_clear_g_free (&conn_info->_loaded_path_etc);
			}
			loaded_dirname = priv->dirname_run;
			loaded_path = g_steal_pointer (&conn_info->_loaded_path_run);
		} else if (conn_info->_loaded_path_etc) {
			loaded_dirname = priv->dirname_etc;
			loaded_path = g_steal_pointer (&conn_info->_loaded_path_etc);
		}
		nm_assert ((!loaded_path) == (!loaded_dirname));

		/* sort storage datas by priority. */
		c_list_sort (&conn_info->cisd_lst_head, _conn_info_storage_data_cmp, NULL);

		if (loaded_path) {
			if (nm_streq (loaded_path, NM_KEYFILE_PATH_NMLOADED_NULL)) {
				loaded_path_masked = TRUE;
				nm_clear_g_free (&loaded_path);
			} else if (!_conn_info_storage_data_prioritize_loaded (&conn_info->cisd_lst_head, loaded_path)) {
				gs_free char *f1 = NULL;

				_LOGT ("load: \"%s\": ignore invalid target \"%s\"",
				       (f1 = nms_keyfile_loaded_uuid_filename (loaded_dirname, conn_info->uuid, FALSE)),
				       loaded_path);
				nm_clear_g_free (&loaded_path);
			}
		}

		if (conn_info->storage_type_exported == NMS_KEYFILE_STORAGE_TYPE_MEM) {

			/* this profile has an in-memory connection (already from earlier). It cannot be
			 * modified during a re-load, because the in-memory connection is no longer tied to
			 * a profile on disk. It just happens that a profile on disk and an in-memory profile
			 * share the same UUID. Reloading the file cannot update the (detached) in-memory connection
			 * of the same UUID. */

			nm_assert (NM_IS_CONNECTION (conn_info->connection_exported));
			if (loaded_path_masked) {
				gs_free char *f1 = NULL;

				_LOGT ("load: \"%s\": masking via %s is ignored due to in-memory profile",
				       (f1 = nms_keyfile_loaded_uuid_filename (loaded_dirname, conn_info->uuid, FALSE)),
				       NM_KEYFILE_PATH_NMLOADED_NULL);
			}
			c_list_for_each_entry (sd, &conn_info->cisd_lst_head, cisd_lst) {
				if (loaded_path) {
					gs_free char *f1 = NULL;

					_LOGT ("load: \"%s\": profile %s shadowed by in-memory profile (was hinted by \"%s\")",
					       sd->full_filename,
					       conn_info->uuid,
					       (f1 = nms_keyfile_loaded_uuid_filename (loaded_dirname, conn_info->uuid, FALSE)));
					nm_clear_g_free (&loaded_path);
				} else
					_LOGT ("load: \"%s\": profile %s shadowed by in-memory profile", sd->full_filename, conn_info->uuid);
			}
			goto prepare_post;
		}

		sd_best = c_list_first_entry (&conn_info->cisd_lst_head, ConnInfoStorageData, cisd_lst);
		if (!sd_best || loaded_path_masked) {
			gs_free char *f1 = NULL;

			/* after reload, no file references this profile (or the files are masked from loading
			 * via a symlink to /dev/null). */

			if (conn_info->storage) {
				/* the profile was exported, we need to signal that it is gone now. */
				if (!events_del)
					events_del = g_array_new (FALSE, FALSE, sizeof (EventsDelData));
				g_array_append_val (events_del,
				                    ((EventsDelData) {
				                        .uuid = g_strdup (conn_info->uuid),
				                        .storage = g_object_ref (conn_info->storage)
				                    }));
			}
			if (c_list_is_empty (&conn_info->cisd_lst_head)) {
				/* if, and only if, we track no files in the conn_info, we delete
				 * the conn_info instance entirely as it serves no further purpose.
				 * Otherwise, we keep it around, to track which files correspond to this UUID. */
				if (loaded_path_masked) {
					gs_free char *f2 = NULL;

					_LOGT ("load: \"%s\": symlinks %s but there are no profiles with UUID \"%s\"",
					       (f2 = nms_keyfile_loaded_uuid_filename (loaded_dirname, conn_info->uuid, FALSE)),
					       NM_KEYFILE_PATH_NMLOADED_NULL,
					       conn_info->uuid);
				}
				_conn_infos_remove (&priv->conn_infos, conn_info);
				continue;
			}
			c_list_for_each_entry (sd, &conn_info->cisd_lst_head, cisd_lst) {
				_LOGT ("load: \"%s\": profile %s masked by \"%s\" file symlinking %s",
				       sd->full_filename,
				       conn_info->uuid,
				       f1 ?: (f1 = nms_keyfile_loaded_uuid_filename (loaded_dirname, conn_info->uuid, FALSE)),
				       NM_KEYFILE_PATH_NMLOADED_NULL);
			}
			g_clear_object (&conn_info->connection_exported);
			g_clear_object (&conn_info->storage);
			goto prepare_post;
		}

		c_list_for_each_entry (sd, &conn_info->cisd_lst_head, cisd_lst) {
			if (sd_best != sd) {
				_LOGT ("load: \"%s\": profile %s shadowed by \"%s\" file",
				       sd->full_filename,
				       conn_info->uuid,
				       sd_best->full_filename);
			}
		}

		conn_info->storage_type_exported = sd_best->storage_type;
		connection_modified = !_conn_info_has_equal_connection (conn_info, sd_best->connection);
		{
			gs_free char *f1 = NULL;

			_LOGT ("load: \"%s\": profile %s (%s) loaded (%s)%s%s%s",
			       sd_best->full_filename,
			       conn_info->uuid,
			       nm_connection_get_id (sd_best->connection),
			       connection_modified ? (conn_info->connection_exported ? "updated" : "added" ) : "unchanged",
			       NM_PRINT_FMT_QUOTED (loaded_path,
			                            " (hinted by \"",
			                            (f1 = nms_keyfile_loaded_uuid_filename (loaded_dirname, conn_info->uuid, FALSE)),
			                            "\")",
			                            ""));
		}
		if (connection_modified) {
			nm_g_object_ref_set (&conn_info->connection_exported, sd_best->connection);
			if (!events_mod)
				events_mod = g_ptr_array_new_with_free_func (g_free);
			g_ptr_array_add (events_mod, g_strdup (conn_info->uuid));
		}

prepare_post:
		c_list_for_each_entry (sd, &conn_info->cisd_lst_head, cisd_lst) {
			/* these NMConnection instances only serve a purpose while reloading. Drop
			 * it now. There is only one relevant connection instance, and by now that
			 * one is referenced by conn_info->connection_exported.
			 *
			 * We only keep the entire cisd_lst_head list for the filenames which
			 * all belong to this UUID. */
			g_clear_object (&sd->connection);

			if (!nm_g_hash_table_insert (priv->conn_infos.filename_idx,
			                             sd->full_filename,
			                             conn_info)) {
				/* There must be no duplicates here. However, we don't assert against that,
				 * because it would mean to assert that readdir() syscall never returns duplicates.
				 * We don't want to assert against kernel IO results. */
			}
		}
	}

	/* raise events. */
	if (events_del) {
		for (i = 0; i < events_del->len; i++) {
			EventsDelData *e = &g_array_index (events_del, EventsDelData, i);

			_nm_settings_plugin_emit_signal_connection_changed (NM_SETTINGS_PLUGIN (self),
			                                                    e->uuid,
			                                                    NM_SETTINGS_STORAGE (e->storage),
			                                                    NULL);
			nm_clear_g_free (&e->uuid);
			g_clear_object (&e->storage);
		}
	}
	if (events_mod) {
		for (i = 0; i < events_mod->len; i++) {
			const char *uuid = events_mod->pdata[i];

			conn_info = _conn_infos_get (&priv->conn_infos, uuid);
			if (   !conn_info
			    || !conn_info->connection_exported) {
				/* hm? The profile was deleted in the meantime? That is only possible
				 * if the signal handler called again into the plugin. Ignore it. */
				continue;
			}

			_conn_info_ensure_storage (self, conn_info);

			_nm_settings_plugin_emit_signal_connection_changed (NM_SETTINGS_PLUGIN (self),
			                                                    uuid,
			                                                    NM_SETTINGS_STORAGE (conn_info->storage),
			                                                    conn_info->connection_exported);
		}
	}
}

static gboolean
_do_load_connection (NMSKeyfilePlugin *self,
                     const char *full_filename,
                     NMSettingsStorage **out_storage,
                     NMConnection **out_connection,
                     GError **error)
{
	NMSKeyfilePluginPrivate *priv = NMS_KEYFILE_PLUGIN_GET_PRIVATE (self);
	NMSKeyfileStorageType storage_type;
	const char *dirname;
	const char *filename;
	NMSKeyfileConnInfo *conn_info;
	gs_unref_object NMConnection *connection_new = NULL;
	gboolean connection_modified;
	GError *local = NULL;
	gboolean loaded_uuid_success;
	gs_free char *loaded_uuid_filename = NULL;

	nm_assert (!out_storage || !*out_storage);
	nm_assert (!out_connection || !*out_connection);

	if (!_path_detect_storage_type (full_filename,
	                                NM_CAST_STRV_CC (priv->dirname_libs),
	                                priv->dirname_etc,
	                                priv->dirname_run,
	                                &storage_type,
	                                &dirname,
	                                &filename,
	                                error))
		return FALSE;

	connection_new = _read_from_file (full_filename, _get_plugin_dir (priv), NULL, &local);
	if (!connection_new) {
		_LOGT ("load: \"%s\": failed to load connection: %s", full_filename, local->message);
		g_propagate_error (error, local);
		return FALSE;
	}

	conn_info = _conn_infos_add (&priv->conn_infos, nm_connection_get_uuid (connection_new));

	connection_modified = !_conn_info_has_equal_connection (conn_info, connection_new);

	/* contray to reload-all, an explicit load can also replace an in-memory connection
	 * with a profile from disk. */
	conn_info->storage_type_exported = storage_type;

	_LOGT ("load: \"%s/%s\": profile %s (%s) loaded (%s)",
	       dirname,
	       filename,
	       conn_info->uuid,
	       nm_connection_get_id (connection_new),
	       connection_modified ? (conn_info->connection_exported ? "updated" : "added" ) : "unchanged");

	/* mark the profile as loaded, so that it's still used after restart.
	 *
	 * For the moment, only do this in the /run directory, meaning the
	 * information is lost after reboot.
	 *
	 * In the future, maybe we can be smarter about this and persist it to /etc,
	 * so that the preferred loaded file is still preferred after reboot. */
	loaded_uuid_success = nms_keyfile_loaded_uuid_write (priv->dirname_run,
	                                                     conn_info->uuid,
	                                                     full_filename,
	                                                     TRUE,
	                                                     &loaded_uuid_filename);
	_LOGT ("load: %s writing loaded file \"%s\" symlinking \"%s\"",
	       loaded_uuid_success ? "success" : "failure",
	       loaded_uuid_filename,
	       full_filename);

	_conn_infos_register_filename (&priv->conn_infos,
	                               conn_info,
	                               dirname,
	                               filename);

	_conn_info_ensure_storage (self, conn_info);

	NM_SET_OUT (out_storage, NM_SETTINGS_STORAGE (g_object_ref (conn_info->storage)));

	if (connection_modified) {
		NM_SET_OUT (out_connection, g_object_ref (connection_new));
		nm_g_object_ref_set (&conn_info->connection_exported, connection_new);
		_nm_settings_plugin_emit_signal_connection_changed (NM_SETTINGS_PLUGIN (self),
		                                                    conn_info->uuid,
		                                                    NM_SETTINGS_STORAGE (conn_info->storage),
		                                                    conn_info->connection_exported);
	} else
		NM_SET_OUT (out_connection, g_object_ref (conn_info->connection_exported));

	return TRUE;
}

static gboolean
_do_commit_changes (NMSKeyfilePlugin *self,
                    NMSKeyfileConnInfo *conn_info,
                    NMConnection *connection,
                    NMSettingsStorageCommitReason commit_reason,
                    GError **error)
{
	NMSKeyfilePluginPrivate *priv;
	gboolean connection_modified;
	/* FIXME: for the moment, we may clone the connection. Fix that to keep connections
	 * immutable and only reference them. */
	gs_unref_object NMConnection *connection_clone = NULL;
	ConnInfoStorageData *sd;
#if 0
	//XXX
	NMSKeyfilePlugin *self;
	NMSKeyfileConnInfo *conn_info;
#endif

	nm_assert (NMS_IS_KEYFILE_PLUGIN (self));
	nm_assert (NMS_IS_KEYFILE_STORAGE (conn_info));
	nm_assert (NMS_IS_KEYFILE_STORAGE (conn_info->storage));
	nm_assert (nm_settings_storage_get_plugin (NM_SETTINGS_STORAGE (conn_info->storage)) == NM_SETTINGS_PLUGIN (self));
	nm_assert (NM_IS_CONNECTION (connection));
	nm_assert (out_reread_connection && !*out_reread_connection);
	nm_assert (!out_logmsg_change || !*out_logmsg_change);

	priv = NMS_KEYFILE_PLUGIN_GET_PRIVATE (self);

	connection_modified = !_conn_info_has_equal_connection (conn_info, connection_new);

	if (conn_info->storage_type == NMS_KEYFILE_STORAGE_TYPE_MEM) {
		if (connection_modified) {
			connection_clone = nm_simple_connection_new_clone (connection);
			nm_g_object_ref_set (&conn_info->connection_exported, connection_clone);
			_nm_settings_plugin_emit_signal_connection_changed (NM_SETTINGS_PLUGIN (self),
			                                                    conn_info->uuid,
			                                                    NM_SETTINGS_STORAGE (conn_info->storage),
			                                                    conn_info->connection_exported);
		}
	} else {
		gs_unref_object NMConnection *reread = NULL;
		gboolean reread_same = FALSE;
		const char *dirname;
		const char *filename;
		gboolean existing_path_read_only = FALSE;
		gboolean require_loaded_uuid_in_etc = FALSE;

		sd = c_list_first_entry (&conn_info->cisd_lst_head, ConnInfoStorageData, cisd_lst);
		nm_assert (sd);

		if (!_path_detect_storage_type (sd->full_filename,
		                                NM_CAST_STRV_CC (priv->dirname_libs),
		                                priv->dirname_etc,
		                                priv->dirname_run,
		                                &storage_type,
		                                &dirname,
		                                &filename,
		                                error))
			return FALSE;

		if (storage_type == NMS_KEYFILE_STORAGE_TYPE_LIB) {
			existing_path_read_only = TRUE;
			if (priv->dirname_etc) {
				storage_type = NMS_KEYFILE_STORAGE_TYPE_ETC;
				dirname = priv->dirname_etc;
				require_loaded_uuid_in_etc = TRUE;
			} else {
				storage_type = NMS_KEYFILE_STORAGE_TYPE_RUN;
				dirname = priv->dirname_run;
			}
		}

		if (!nms_keyfile_writer_connection (connection,
		                                    dirname,
		                                    _get_plugin_dir (priv),
		                                    sd->full_filename,
		                                    existing_path_read_only,
		                                    NM_FLAGS_ALL (commit_reason,   NM_SETTINGS_STORAGE_COMMIT_REASON_USER_ACTION
		                                                                 | NM_SETTINGS_STORAGE_COMMIT_REASON_ID_CHANGED),
		                                    &filename,
		                                    &reread,
		                                    &reread_same,
		                                    error))
			return FALSE;

#if 0
		if (!nm_streq0 (filename, priv->filename)) {
			gs_free char *old_filename = g_steal_pointer (&priv->filename);

			priv->filename = g_steal_pointer (&filename);

			if (old_filename) {
				NM_SET_OUT (out_logmsg_change,
				            g_strdup_printf ("keyfile: update \"%s\" (\"%s\", %s) and rename from \"%s\"",
				                             priv->filename,
				                             nm_connection_get_id (connection),
				                             nm_connection_get_uuid (connection),
				                             old_filename));
			} else {
				NM_SET_OUT (out_logmsg_change,
				            g_strdup_printf ("keyfile: update \"%s\" (\"%s\", %s) and persist connection",
				                             priv->filename,
				                             nm_connection_get_id (connection),
				                             nm_connection_get_uuid (connection)));
			}
		} else {
			NM_SET_OUT (out_logmsg_change,
			            g_strdup_printf ("keyfile: update \"%s\" (\"%s\", %s)",
			                             priv->filename,
			                             nm_connection_get_id (connection),
			                             nm_connection_get_uuid (connection)));
		}

		if (reread && !reread_same)
			*out_reread_connection = g_steal_pointer (&reread);
#endif

	return TRUE;
}

static gboolean
_do_delete (NMSKeyfilePlugin *self,
            NMSKeyfileConnInfo *conn_info,
            GError **error)
{
	//XXX
	return FALSE;
}

static gboolean
_do_add_connection (NMSKeyfilePlugin *self,
                    NMConnection *connection,
                    gboolean save_to_disk,
                    NMSettingsStorage **out_storage,
                    NMConnection **out_connection,
                    GError **error)
{
#if 0
//XXX
	NMSKeyfilePlugin *self = NMS_KEYFILE_PLUGIN (config);
	gs_free char *path = NULL;
	gs_unref_object NMConnection *reread = NULL;

	if (!nms_keyfile_writer_connection (connection,
	                                    save_to_disk,
	                                    NULL,
	                                    FALSE,
	                                    &path,
	                                    &reread,
	                                    NULL,
	                                    error))
		return NULL;

	return NM_SETTINGS_CONNECTION (update_connection (self, reread ?: connection, path, NULL, FALSE, NULL, error));
#endif
	return FALSE;
}

/*****************************************************************************/

static void
config_changed_cb (NMConfig *config,
                   NMConfigData *config_data,
                   NMConfigChangeFlags changes,
                   NMConfigData *old_data,
                   NMSKeyfilePlugin *self)
{
	gs_free char *old_value = NULL;
	gs_free char *new_value = NULL;

	old_value = nm_config_data_get_value (old_data,    NM_CONFIG_KEYFILE_GROUP_KEYFILE, NM_CONFIG_KEYFILE_KEY_KEYFILE_UNMANAGED_DEVICES, NM_CONFIG_GET_VALUE_TYPE_SPEC);
	new_value = nm_config_data_get_value (config_data, NM_CONFIG_KEYFILE_GROUP_KEYFILE, NM_CONFIG_KEYFILE_KEY_KEYFILE_UNMANAGED_DEVICES, NM_CONFIG_GET_VALUE_TYPE_SPEC);

	if (!nm_streq0 (old_value, new_value))
		_nm_settings_plugin_emit_signal_unmanaged_specs_changed (NM_SETTINGS_PLUGIN (self));
}

static GSList *
get_unmanaged_specs (NMSettingsPlugin *config)
{
	NMSKeyfilePluginPrivate *priv = NMS_KEYFILE_PLUGIN_GET_PRIVATE (config);
	gs_free char *value = NULL;

	value = nm_config_data_get_value (nm_config_get_data (priv->config),
	                                  NM_CONFIG_KEYFILE_GROUP_KEYFILE,
	                                  NM_CONFIG_KEYFILE_KEY_KEYFILE_UNMANAGED_DEVICES,
	                                  NM_CONFIG_GET_VALUE_TYPE_SPEC);
	return nm_match_spec_split (value);
}

/*****************************************************************************/

#if 0
static void
connection_removed_cb (NMSettingsConnection *sett_conn, NMSKeyfilePlugin *self)
{
	g_hash_table_remove (NMS_KEYFILE_PLUGIN_GET_PRIVATE (self)->conn_infos,
	                     nm_settings_connection_get_uuid (sett_conn));
}

/* Monitoring */

static void
remove_connection (NMSKeyfilePlugin *self, NMSKeyfileConnection *connection)
{
	gboolean removed;

	g_return_if_fail (connection != NULL);

	_LOGI ("removed " NMS_KEYFILE_CONNECTION_LOG_FMT, NMS_KEYFILE_CONNECTION_LOG_ARG (connection));

	/* Removing from the hash table should drop the last reference */
	g_object_ref (connection);
	g_signal_handlers_disconnect_by_func (connection, connection_removed_cb, self);
	removed = g_hash_table_remove (NMS_KEYFILE_PLUGIN_GET_PRIVATE (self)->conn_infos,
	                               nm_settings_connection_get_uuid (NM_SETTINGS_CONNECTION (connection)));
	nm_settings_connection_signal_remove (NM_SETTINGS_CONNECTION (connection));
	g_object_unref (connection);

	g_return_if_fail (removed);
}

static NMSKeyfileConnection *
find_by_path (NMSKeyfilePlugin *self, const char *path)
{
	NMSKeyfilePluginPrivate *priv = NMS_KEYFILE_PLUGIN_GET_PRIVATE (self);
	GHashTableIter iter;
	NMSettingsConnection *candidate = NULL;

	g_return_val_if_fail (path != NULL, NULL);

	g_hash_table_iter_init (&iter, priv->conn_infos);
	while (g_hash_table_iter_next (&iter, NULL, (gpointer) &candidate)) {
		if (g_strcmp0 (path, nm_settings_connection_get_filename (candidate)) == 0)
			return NMS_KEYFILE_CONNECTION (candidate);
	}
	return NULL;
}

/* update_connection:
 * @self: the plugin instance
 * @source: if %NULL, this re-reads the connection from @full_path
 *   and updates it. When passing @source, this adds a connection from
 *   memory.
 * @full_path: the filename of the keyfile to be loaded
 * @connection: an existing connection that might be updated.
 *   If given, @connection must be an existing connection that is currently
 *   owned by the plugin.
 * @protect_existing_connection: if %TRUE, and !@connection, we don't allow updating
 *   an existing connection with the same UUID.
 *   If %TRUE and @connection, allow updating only if the reload would modify
 *   @connection (without changing its UUID) or if we would create a new connection.
 *   In other words, if this parameter is %TRUE, we only allow creating a
 *   new connection (with an unseen UUID) or updating the passed in @connection
 *   (whereas the UUID cannot change).
 *   Note, that this allows for @connection to be replaced by a new connection.
 * @protected_connections: (allow-none): if given, we only update an
 *   existing connection if it is not contained in this hash.
 * @error: error in case of failure
 *
 * Loads a connection from file @full_path. This can both be used to
 * load a connection initially or to update an existing connection.
 *
 * If you pass in an existing connection and the reloaded file happens
 * to have a different UUID, the connection is deleted.
 * Beware, that means that after the function, you have a dangling pointer
 * if the returned connection is different from @connection.
 *
 * Returns: the updated connection.
 * */
static NMSKeyfileConnection *
update_connection (NMSKeyfilePlugin *self,
                   NMConnection *source,
                   const char *full_path,
                   NMSKeyfileConnection *connection,
                   gboolean protect_existing_connection,
                   GHashTable *protected_connections,
                   GError **error)
{
	NMSKeyfilePluginPrivate *priv = NMS_KEYFILE_PLUGIN_GET_PRIVATE (self);
	NMSKeyfileConnection *connection_new;
	NMSKeyfileConnection *connection_by_uuid;
	GError *local = NULL;
	const char *uuid;

	g_return_val_if_fail (!source || NM_IS_CONNECTION (source), NULL);
	g_return_val_if_fail (full_path || source, NULL);

	if (full_path)
		_LOGD ("loading from file \"%s\"...", full_path);

	if (   !nm_utils_file_is_in_path (full_path, nms_keyfile_utils_get_path ())
	    && !nm_utils_file_is_in_path (full_path, NM_KEYFILE_PATH_NAME_RUN)) {
		g_set_error_literal (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_FAILED,
		                     "File not in recognized system-connections directory");
		return FALSE;
	}

	connection_new = nms_keyfile_connection_new (source, full_path, nms_keyfile_utils_get_path (), &local);
	if (!connection_new) {
		/* Error; remove the connection */
		if (source)
			_LOGW ("error creating connection %s: %s", nm_connection_get_uuid (source), local->message);
		else
			_LOGW ("error loading connection from file %s: %s", full_path, local->message);
		if (   connection
		    && !protect_existing_connection
		    && (!protected_connections || !g_hash_table_contains (protected_connections, connection)))
			remove_connection (self, connection);
		g_propagate_error (error, local);
		return NULL;
	}

	uuid = nm_settings_connection_get_uuid (NM_SETTINGS_CONNECTION (connection_new));
	connection_by_uuid = g_hash_table_lookup (priv->conn_infos, uuid);

	if (   connection
	    && connection != connection_by_uuid) {

		if (   (protect_existing_connection && connection_by_uuid != NULL)
		    || (protected_connections && g_hash_table_contains (protected_connections, connection))) {
			NMSKeyfileConnection *conflicting = (protect_existing_connection && connection_by_uuid != NULL) ? connection_by_uuid : connection;

			if (source)
				_LOGW ("cannot update protected "NMS_KEYFILE_CONNECTION_LOG_FMT" connection due to conflicting UUID %s", NMS_KEYFILE_CONNECTION_LOG_ARG (conflicting), uuid);
			else
				_LOGW ("cannot load %s due to conflicting UUID for "NMS_KEYFILE_CONNECTION_LOG_FMT, full_path, NMS_KEYFILE_CONNECTION_LOG_ARG (conflicting));
			g_object_unref (connection_new);
			g_set_error_literal (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_FAILED,
			                      "Cannot update protected connection due to conflicting UUID");
			return NULL;
		}

		/* The new connection has a different UUID then the original one.
		 * Remove @connection. */
		remove_connection (self, connection);
	}

	if (   connection_by_uuid
	    && (   (!connection && protect_existing_connection)
	        || (protected_connections && g_hash_table_contains (protected_connections, connection_by_uuid)))) {
		if (source)
			_LOGW ("cannot update connection due to conflicting UUID for "NMS_KEYFILE_CONNECTION_LOG_FMT, NMS_KEYFILE_CONNECTION_LOG_ARG (connection_by_uuid));
		else
			_LOGW ("cannot load %s due to conflicting UUID for "NMS_KEYFILE_CONNECTION_LOG_FMT, full_path, NMS_KEYFILE_CONNECTION_LOG_ARG (connection_by_uuid));
		g_object_unref (connection_new);
		g_set_error_literal (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_FAILED,
		                      "Skip updating protected connection during reload");
		return NULL;
	}

	if (connection_by_uuid) {
		const char *old_path;

		old_path = nm_settings_connection_get_filename (NM_SETTINGS_CONNECTION (connection_by_uuid));

		if (nm_connection_compare (nm_settings_connection_get_connection (NM_SETTINGS_CONNECTION (connection_by_uuid)),
		                           nm_settings_connection_get_connection (NM_SETTINGS_CONNECTION (connection_new)),
		                           NM_SETTING_COMPARE_FLAG_IGNORE_AGENT_OWNED_SECRETS |
		                           NM_SETTING_COMPARE_FLAG_IGNORE_NOT_SAVED_SECRETS)) {
			/* Nothing to do... except updating the path. */
			if (old_path && g_strcmp0 (old_path, full_path) != 0)
				_LOGI ("rename \"%s\" to "NMS_KEYFILE_CONNECTION_LOG_FMT" without other changes", old_path, NMS_KEYFILE_CONNECTION_LOG_ARG (connection_new));
		} else {
			/* An existing connection changed. */
			if (source)
				_LOGI ("update "NMS_KEYFILE_CONNECTION_LOG_FMT" from %s", NMS_KEYFILE_CONNECTION_LOG_ARG (connection_new), NMS_KEYFILE_CONNECTION_LOG_PATH (old_path));
			else if (!g_strcmp0 (old_path, nm_settings_connection_get_filename (NM_SETTINGS_CONNECTION (connection_new))))
				_LOGI ("update "NMS_KEYFILE_CONNECTION_LOG_FMT, NMS_KEYFILE_CONNECTION_LOG_ARG (connection_new));
			else if (old_path)
				_LOGI ("rename \"%s\" to "NMS_KEYFILE_CONNECTION_LOG_FMT, old_path, NMS_KEYFILE_CONNECTION_LOG_ARG (connection_new));
			else
				_LOGI ("update and persist "NMS_KEYFILE_CONNECTION_LOG_FMT, NMS_KEYFILE_CONNECTION_LOG_ARG (connection_new));

			if (!nm_settings_connection_update (NM_SETTINGS_CONNECTION (connection_by_uuid),
			                                    nm_settings_connection_get_connection (NM_SETTINGS_CONNECTION (connection_new)),
			                                    NM_SETTINGS_CONNECTION_PERSIST_MODE_KEEP_SAVED,
			                                    NM_SETTINGS_CONNECTION_COMMIT_REASON_NONE,
			                                    "keyfile-update",
			                                    &local)) {
				/* Shouldn't ever get here as 'connection_new' was verified by the reader already
				 * and the UUID did not change. */
				g_assert_not_reached ();
			}
			g_assert_no_error (local);
		}
		nm_settings_connection_set_filename (NM_SETTINGS_CONNECTION (connection_by_uuid), full_path);
		g_object_unref (connection_new);
		return connection_by_uuid;
	} else {
		if (source)
			_LOGI ("add connection "NMS_KEYFILE_CONNECTION_LOG_FMT, NMS_KEYFILE_CONNECTION_LOG_ARG (connection_new));
		else
			_LOGI ("new connection "NMS_KEYFILE_CONNECTION_LOG_FMT, NMS_KEYFILE_CONNECTION_LOG_ARG (connection_new));
		g_hash_table_insert (priv->conn_infos, g_strdup (uuid), connection_new);

		g_signal_connect (connection_new, NM_SETTINGS_CONNECTION_REMOVED,
		                  G_CALLBACK (connection_removed_cb),
		                  self);

		if (!source) {
			/* Only raise the signal if we were called without source, i.e. if we read the connection from file.
			 * Otherwise, we were called by add_connection() which does not expect the signal. */
			_nm_settings_plugin_emit_signal_connection_added (NM_SETTINGS_PLUGIN (self),
			                                                  NM_SETTINGS_CONNECTION (connection_new));
		}

		return connection_new;
	}
}
#endif

/*****************************************************************************/

static void
_monitoring_dir_changed (GFileMonitor *monitor,
                         GFile *file,
                         GFile *other_file,
                         GFileMonitorEvent event_type,
                         gpointer user_data)
{
	NMSKeyfilePlugin *self = NMS_KEYFILE_PLUGIN (user_data);
	NMSKeyfilePluginPrivate *priv = NMS_KEYFILE_PLUGIN_GET_PRIVATE (self);
	NMConnection *connection;
	gs_free char *full_filename = NULL;
	NMSKeyfileStorageType storage_type;
	const char *dirname;
	const char *filename;
	NMSKeyfileConnInfo *conn_info;
	gs_free char *normalized_filename = NULL;

	if (!priv->initialized)
		return;

	full_filename = g_file_get_path (file);

	if (!_path_detect_storage_type (full_filename,
	                                NM_CAST_STRV_CC (priv->dirname_libs),
	                                priv->dirname_etc,
	                                priv->dirname_run,
	                                &storage_type,
	                                &dirname,
	                                &filename,
	                                NULL))
		return;

	normalized_filename = g_strdup_printf ("%s/%s", dirname, filename);

	connection = _read_from_file (normalized_filename, _get_plugin_dir (priv), NULL, NULL);

	_LOGT ("load: file \"%s\" changed: %d, %s%s%s",
	       normalized_filename,
	       event_type,
	       NM_PRINT_FMT_QUOTED (connection, "loaded \"", nm_connection_get_uuid (connection), "\"", "no connection"));

	conn_info = g_hash_table_lookup (priv->conn_infos.filename_idx, normalized_filename);

	switch (event_type) {
	case G_FILE_MONITOR_EVENT_DELETED:
		// XXX: todo
		(void) conn_info;
		break;
	case G_FILE_MONITOR_EVENT_CREATED:
	case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
		// XXX: todo
		break;
	default:
		break;
	}
}

static void
_monitoring_clear (NMSKeyfilePlugin *self)
{
	NMSKeyfilePluginPrivate *priv = NMS_KEYFILE_PLUGIN_GET_PRIVATE (self);
	guint i;

	if (!priv->monitors)
		return;

	for (i = 0; priv->monitors[i]; i++) {
		g_signal_handlers_disconnect_by_func (priv->monitors[i], _monitoring_dir_changed, self);
		g_file_monitor_cancel (priv->monitors[i]);
		g_clear_object (&priv->monitors[i]);
	}
	nm_clear_g_free (&priv->monitors);
}

static GFileMonitor *
_monitoring_setup_one (NMSKeyfilePlugin *self,
                       const char *dirname)
{
	GFileMonitor *monitor;
	gs_unref_object GFile *file = NULL;

	nm_assert (dirname);

	file = g_file_new_for_path (dirname);

	monitor = g_file_monitor_directory (file, G_FILE_MONITOR_NONE, NULL, NULL);
	if (monitor)
		g_signal_connect (monitor, "changed", G_CALLBACK (_monitoring_dir_changed), self);

	return monitor;
}

static void
_monitoring_setup (NMSKeyfilePlugin *self)
{
	NMSKeyfilePluginPrivate *priv = NMS_KEYFILE_PLUGIN_GET_PRIVATE (self);
	GFileMonitor **monitors;
	GFileMonitor *monitor;
	guint n_dirname_libs;
	guint i, j;

	_monitoring_clear (self);

	if (!nm_config_get_monitor_connection_files (priv->config))
		return;

	n_dirname_libs = NM_PTRARRAY_LEN (priv->dirname_libs);

	monitors = g_new (GFileMonitor *, n_dirname_libs + 3);

	j = 0;
	for (i = 0; i < n_dirname_libs; i++) {
		if ((monitor = _monitoring_setup_one (self, priv->dirname_libs[i])))
			monitors[j++] = monitor;
	}
	if ((monitor = _monitoring_setup_one (self, priv->dirname_etc)))
		monitors[j++] = monitor;
	if ((monitor = _monitoring_setup_one (self, priv->dirname_run)))
		monitors[j++] = monitor;

	monitors[j] = NULL;

	nm_assert (j < n_dirname_libs + 3);

	priv->monitors = monitors;
}

/*****************************************************************************/

static void
reload_connections (NMSettingsPlugin *plugin)
{
	_do_reload_all (NMS_KEYFILE_PLUGIN (plugin));
}

static gboolean
load_connection (NMSettingsPlugin *plugin,
                 const char *filename,
                 NMSettingsStorage **out_storage,
                 NMConnection **out_connection,
                 GError **error)
{
	return _do_load_connection (NMS_KEYFILE_PLUGIN (plugin),
	                            filename,
	                            out_storage,
	                            out_connection,
	                            error);
}

static gboolean
add_connection (NMSettingsPlugin *config,
                NMConnection *connection,
                gboolean save_to_disk,
                NMSettingsStorage **out_storage,
                NMConnection **out_connection,
                GError **error)
{
	return _do_add_connection (NMS_KEYFILE_PLUGIN (config),
	                           connection,
	                           save_to_disk,
	                           out_storage,
	                           out_connection,
	                           error);
}

static gboolean
commit_changes (NMSettingsPlugin *plugin,
                NMSettingsStorage *storage,
                NMConnection *connection,
                NMSettingsStorageCommitReason commit_reason,
                //XXX unused
                NMConnection **out_reread_connection,
                char **out_logmsg_change,
                GError **error)
{
	NMSKeyfilePlugin *self = NMS_KEYFILE_PLUGIN (plugin);
	NMSKeyfileConnInfo *conn_info;

	conn_info = _conn_info_from_storage (self, storage, error);
	if (!conn_info)
		g_return_val_if_reached (FALSE);
	return _do_commit_changes (self,
	                           conn_info,
	                           connection,
	                           commit_reason,
	                           error);
}

static gboolean
delete (NMSettingsPlugin *plugin,
        NMSettingsStorage *storage,
        GError **error)
{
	NMSKeyfilePlugin *self = NMS_KEYFILE_PLUGIN (plugin);
	NMSKeyfileConnInfo *conn_info;

	conn_info = _conn_info_from_storage (self, storage, error);
	if (!conn_info)
		g_return_val_if_reached (FALSE);
	return _do_delete (self, conn_info, error);
}

/*****************************************************************************/

static void
nms_keyfile_plugin_init (NMSKeyfilePlugin *plugin)
{
	NMSKeyfilePluginPrivate *priv = NMS_KEYFILE_PLUGIN_GET_PRIVATE (plugin);

	priv->config = g_object_ref (nm_config_get ());

	c_list_init (&priv->conn_infos.lst_head);
	priv->conn_infos.filename_idx = g_hash_table_new (nm_str_hash, g_str_equal);
	priv->conn_infos.idx = g_hash_table_new_full (nm_pstr_hash, nm_pstr_equal, (GDestroyNotify) _conn_info_destroy, NULL);

	/* dirname_libs are a set of read-only directories with lower priority than /etc or /run.
	 * There is nothing complicated about having multiple of such directories, so dirname_libs
	 * is a list (which currently only has at most one directory). */
	priv->dirname_libs = g_new0 (char *, 2);
	priv->dirname_libs[0] = nm_sd_utils_path_simplify (g_strdup (NM_KEYFILE_PATH_NAME_LIB), FALSE);
	priv->dirname_run = nm_sd_utils_path_simplify (g_strdup (NM_KEYFILE_PATH_NAME_RUN), FALSE);
	priv->dirname_etc = nm_config_data_get_value (NM_CONFIG_GET_DATA_ORIG,
	                                              NM_CONFIG_KEYFILE_GROUP_KEYFILE,
	                                              NM_CONFIG_KEYFILE_KEY_KEYFILE_PATH,
	                                              NM_CONFIG_GET_VALUE_STRIP);
	if (priv->dirname_etc && priv->dirname_etc[0] == '\0') {
		/* special case: configure an empty keyfile path so that NM has no writable keyfile
		 * directory. In this case, NM will only honor dirname_libs and dirname_run, meaning
		 * it cannot persist profile to non-volatile memory. */
		nm_clear_g_free (&priv->dirname_etc);
	} else if (!priv->dirname_etc || priv->dirname_etc[0] != '/') {
		/* either invalid path or unspecified. Use the default. */
		g_free (priv->dirname_etc);
		priv->dirname_etc = nm_sd_utils_path_simplify (g_strdup (NM_KEYFILE_PATH_NAME_ETC_DEFAULT), FALSE);
	} else
		nm_sd_utils_path_simplify (priv->dirname_etc, FALSE);

	/* no duplicates */
	if (NM_IN_STRSET (priv->dirname_libs[0], priv->dirname_etc, priv->dirname_run))
		nm_clear_g_free (&priv->dirname_libs[0]);
	if (NM_IN_STRSET (priv->dirname_etc, priv->dirname_run))
		nm_clear_g_free (&priv->dirname_etc);
}

static void
constructed (GObject *object)
{
	NMSKeyfilePlugin *self = NMS_KEYFILE_PLUGIN (object);
	NMSKeyfilePluginPrivate *priv = NMS_KEYFILE_PLUGIN_GET_PRIVATE (self);

	G_OBJECT_CLASS (nms_keyfile_plugin_parent_class)->constructed (object);

	if (nm_config_data_has_value (nm_config_get_data_orig (priv->config),
	                              NM_CONFIG_KEYFILE_GROUP_KEYFILE,
	                              NM_CONFIG_KEYFILE_KEY_KEYFILE_HOSTNAME,
	                              NM_CONFIG_GET_VALUE_RAW))
		_LOGW ("'hostname' option is deprecated and has no effect");

	g_signal_connect (G_OBJECT (priv->config),
	                  NM_CONFIG_SIGNAL_CONFIG_CHANGED,
	                  G_CALLBACK (config_changed_cb),
	                  self);

	_monitoring_setup (self);
}

NMSKeyfilePlugin *
nms_keyfile_plugin_new (void)
{
	return g_object_new (NMS_TYPE_KEYFILE_PLUGIN, NULL);
}

static void
dispose (GObject *object)
{
	NMSKeyfilePlugin *self = NMS_KEYFILE_PLUGIN (object);
	NMSKeyfilePluginPrivate *priv = NMS_KEYFILE_PLUGIN_GET_PRIVATE (self);

	_monitoring_clear (self);

	nm_clear_pointer (&priv->conn_infos.filename_idx, g_hash_table_destroy);
	nm_clear_pointer (&priv->conn_infos.idx, g_hash_table_destroy);

	if (priv->config) {
		g_signal_handlers_disconnect_by_func (priv->config, config_changed_cb, object);
		g_clear_object (&priv->config);
	}

	nm_clear_pointer (&priv->dirname_libs, g_strfreev);
	nm_clear_g_free (&priv->dirname_etc);
	nm_clear_g_free (&priv->dirname_run);

	G_OBJECT_CLASS (nms_keyfile_plugin_parent_class)->dispose (object);
}

static void
nms_keyfile_plugin_class_init (NMSKeyfilePluginClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	NMSettingsPluginClass *plugin_class = NM_SETTINGS_PLUGIN_CLASS (klass);

	object_class->constructed = constructed;
	object_class->dispose     = dispose;

	plugin_class->get_unmanaged_specs = get_unmanaged_specs;
	plugin_class->reload_connections  = reload_connections;
	plugin_class->load_connection     = load_connection;
	plugin_class->add_connection      = add_connection;
	plugin_class->commit_changes      = commit_changes;
	plugin_class->delete              = delete;
}
