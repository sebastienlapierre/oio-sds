/*
OpenIO SDS meta0v2
Copyright (C) 2014 Worldine, original work as part of Redcurrant
Copyright (C) 2015 OpenIO, modified as part of OpenIO Software Defined Storage

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as
published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <metautils/lib/metautils.h>
#include <cluster/lib/gridcluster.h>

#include "meta0_remote.h"
#include "meta0_utils.h"

static addr_info_t addr = {{0}};
static gchar namespace[LIMIT_LENGTH_NSNAME] = {0};
static gboolean flag_list = FALSE;
static gboolean flag_get = FALSE;
static gboolean flag_reload = FALSE;
static gboolean flag_reset = FALSE;
static gboolean flag_getmeta1info = FALSE;
static gboolean flag_nocheck = FALSE;
static gboolean flag_destroy_meta1ref = FALSE;
static gboolean flag_destroy_zk_node = FALSE;
static guint8 prefix[2] = {0,0};
static gchar **urls = NULL;

static gboolean
url_check(const gchar *u)
{
	addr_info_t a = {{0}};
	return grid_string_to_addrinfo(u, &a);
}

static gboolean
urlv_check(gchar **urlv)
{
	if (!urlv)
		return FALSE;
	for (gchar **u = urlv; *u ;u++) {
		if (!url_check(*u)) {
			GRID_WARN("Bad address [%s]", *u);
			return FALSE;
		}
	}
	return TRUE;
}

static gboolean
_is_usable_meta0(addr_info_t *m0addr, GSList *exclude)
{
	GSList *l = NULL;
	for (l = exclude; l && l->data; l = l->next) {
		if (addr_info_equal(l->data, m0addr))
			return FALSE;
	}
	return TRUE;
}

static addr_info_t *
meta0_utils_getMeta0addr(gchar *ns, GSList **m0_lst, GSList *exclude)
{
	addr_info_t *a = NULL;
	if (*m0_lst == NULL) {
		GError *err = conscience_get_services(ns, NAME_SRVTYPE_META0, FALSE, m0_lst);
		if (err) {
			GRID_WARN("Failed to get Meta0 addresses for namespace %s: (%d) %s",
					ns, err->code, err->message);
			g_clear_error(&err);
			return NULL;
		}
	}
	GSList *m0;
	for (m0 = *m0_lst; m0 && m0->data; m0 = m0->next) {
		service_info_t *srv = m0->data;
		if (_is_usable_meta0(&(srv->addr), exclude))
			a = &(srv->addr);
	}
	return a;
}

/* FIXME(jfs): why the hel should we manage excluded services ? */
static addr_info_t *
_getMeta0addr(GSList **m0_lst, GSList *exclude)
{
	if (namespace[0])
		return  meta0_utils_getMeta0addr(namespace, m0_lst, exclude);
	if (!exclude)
		return &addr;
	return NULL;
}

static void
dump_and_clean_list(GSList *list)
{
	GRID_INFO("(Start of META0 content)");
	if (list) {
		gboolean first = TRUE;
		GPtrArray *array = meta0_utils_list_to_array(list);
		meta0_utils_list_clean(list);
		g_print("{");
		for (guint i = 0; i < array->len; i++) {
			gchar **v;
			if (NULL != (v = array->pdata[i])) {
				if (!first)
					g_print(",\n");
				guint16 p = i;
				gchar *joined = g_strjoinv("\",\"", v);
				g_print("\"%02X%02X\":[\"%s\"]", ((guint8*)&p)[0],
						((guint8*)&p)[1], joined);
				g_free(joined);
				first = FALSE;
			}
		}
		g_print("}");

		meta0_utils_array_clean(array);
	}
	GRID_INFO("(End of META0 content)");
}

static void
meta0_init_reload(void)
{
	GError *err = NULL;
	GSList *exclude = NULL;
	GSList *m0_lst = NULL;
	addr_info_t *m0addr;

	GRID_INFO("Refreshing the META0 (internal caches reload)");

	m0addr = _getMeta0addr(&m0_lst, exclude);
	while (m0addr) {
		gchar url[STRLEN_ADDRINFO];
		grid_addrinfo_to_string(m0addr, url , sizeof(url));
		err = meta0_remote_cache_refresh(url);
		if (err != NULL) {
			GRID_WARN("META0 [%s] refresh error (%d) : %s", url, err->code, err->message);
			g_clear_error(&err);
			grid_main_set_status(1);
		} else {
			GRID_WARN("META0 [%s] refresh", url);
		}
		exclude = g_slist_prepend(exclude,m0addr);
		m0addr = _getMeta0addr(&m0_lst,exclude);
	}

	g_slist_free_full(m0_lst, (GDestroyNotify)service_info_clean);
}

static void
meta0_init_reset(void)
{
	GError *err = NULL;
	GSList *exclude = NULL;
	GSList *m0_lst = NULL;
	addr_info_t *m0addr;

	GRID_INFO("Resetting the META0 (base flush)");

	m0addr = _getMeta0addr(&m0_lst, exclude);
	while (m0addr) {
		gchar url[STRLEN_ADDRINFO];
		grid_addrinfo_to_string(m0addr, url , sizeof(url));
		err = meta0_remote_cache_reset(url, FALSE);
		if (err != NULL) {
			GRID_WARN("META0 [%s] reset error (%d) : %s", url, err->code, err->message);
			g_clear_error(&err);
			grid_main_set_status (1);
		} else {
			GRID_WARN("META0 [%s] reset", url);
		}
		exclude = g_slist_prepend(exclude,m0addr);
		m0addr = _getMeta0addr(&m0_lst,exclude);
	}

	g_slist_free_full(m0_lst, (GDestroyNotify)service_info_clean);
}

static void
meta0_init_list(void)
{
	GError *err = NULL;
	GSList *exclude = NULL;
	GSList *m0_lst = NULL;
	addr_info_t *m0addr;

	GRID_INFO("Dumping the whole META0");

	m0addr = _getMeta0addr(&m0_lst,exclude);
	while (m0addr) {
		gchar url[STRLEN_ADDRINFO];
		grid_addrinfo_to_string(m0addr, url , sizeof(url));
		GSList *list = NULL;
		err = meta0_remote_get_meta1_all(url, &list);
		if (err != NULL) {
			if (CODE_IS_NETWORK_ERROR(err->code)) {
				if (DEBUG_ENABLED()) {
					GRID_DEBUG("Failed to reach meta0 [%s] : error (%d) : %s",
							url, err->code, err->message);
				}
				exclude = g_slist_prepend(exclude,m0addr);
				m0addr = _getMeta0addr(&m0_lst,exclude);
			} else {
				GRID_WARN("META0 request error (%d) : %s", err->code, err->message);
				m0addr = NULL;
			}
			g_clear_error(&err);
		} else {
			dump_and_clean_list(list);
			goto exit;
		}
	}

	grid_main_set_status (1);
exit:
	g_slist_free_full(m0_lst, (GDestroyNotify)service_info_clean);
}

static void
meta0_init_get(void)
{
	GError *err = NULL;
	GSList *exclude = NULL;
	GSList *m0_lst = NULL;
	addr_info_t *m0addr;

	GRID_INFO("Getting a single META0 entry [%02X%02X]", prefix[0], prefix[1]);

	m0addr = _getMeta0addr(&m0_lst,exclude);
	while (m0addr) {
		gchar url[STRLEN_ADDRINFO];
		grid_addrinfo_to_string(m0addr, url , sizeof(url));
		GSList *list = NULL;
		err = meta0_remote_get_meta1_one(url, prefix, &list);
		if (err != NULL) {
			GRID_WARN("META0 request error (%d) : %s", err->code, err->message);
			if (CODE_IS_NETWORK_ERROR(err->code)) {
				exclude = g_slist_prepend(exclude,m0addr);
				m0addr = _getMeta0addr(&m0_lst,exclude);
			} else {
				m0addr = NULL;
			}
			g_clear_error(&err);
		} else {
			dump_and_clean_list(list);
			goto exit;
		}
	}

	grid_main_set_status(1);
exit:
	g_slist_free_full(m0_lst, (GDestroyNotify)service_info_clean);
}

static void
meta0_init_get_meta1_info(void)
{
	GError *err = NULL;
	GSList *exclude = NULL;
	GSList *m0_lst = NULL;
	addr_info_t *m0addr;

	GRID_INFO("GET all META1 information");

	m0addr = _getMeta0addr(&m0_lst,exclude);
	while (m0addr) {
		gchar url[STRLEN_ADDRINFO];
		grid_addrinfo_to_string(m0addr, url , sizeof(url));
		gchar **result = NULL;
		err = meta0_remote_get_meta1_info(url, &result);

		if (err != NULL) {
			GRID_WARN("META0 request error (%d) : %s", err->code, err->message);
			if (CODE_IS_NETWORK_ERROR(err->code)) {
				exclude = g_slist_prepend(exclude, m0addr);
				m0addr = _getMeta0addr(&m0_lst, exclude);
			} else {
				m0addr = NULL;
			}
			g_clear_error(&err);
		} else {
			if (result != NULL) {
				for (gchar **u = result; *u ;u++)
					g_print("%s\n",*u);
				g_free(result);
			} else {
				GRID_INFO("No meta1 referenced");
			}
			goto exit;
		}
	}

	grid_main_set_status(1);
exit:
	g_slist_free_full(m0_lst, (GDestroyNotify)service_info_clean);
}

static void
meta0_init_destroy_meta1ref(void)
{
	GError *err = NULL;
	GSList *exclude = NULL;
	GSList *m0_lst = NULL;
	addr_info_t *m0addr;

	GRID_INFO("Destroy META1 reference");

	m0addr = _getMeta0addr(&m0_lst,exclude);
	while (m0addr) {
		gchar url[STRLEN_ADDRINFO];
		grid_addrinfo_to_string(m0addr, url , sizeof(url));
		err = meta0_remote_destroy_meta1ref(url, *urls);
		if (err != NULL) {
			GRID_WARN("META0 request error (%d) : %s", err->code, err->message);
			if (CODE_IS_NETWORK_ERROR(err->code)) {
				exclude = g_slist_prepend(exclude,m0addr);
				m0addr = _getMeta0addr(&m0_lst,exclude);
			} else {
				m0addr = NULL;
			}
			g_clear_error(&err);
		} else {
			GRID_INFO("META1 reference removed!");
			goto exit;
		}
	}

	grid_main_set_status(1);
exit:
	g_slist_free_full(m0_lst, (GDestroyNotify)service_info_clean);
}

static void
meta0_init_destroy_zk_node(void)
{
	GError *err = NULL;
	GSList *exclude = NULL;
	GSList *m0_lst = NULL;
	addr_info_t *m0addr;

	GRID_INFO("REMOVE META0 Zookeeper node");

	m0addr = _getMeta0addr(&m0_lst,exclude);
	while (m0addr) {
		gchar url[STRLEN_ADDRINFO];
		grid_addrinfo_to_string(m0addr, url , sizeof(url));
		err = meta0_remote_destroy_meta0zknode(url, *urls);
		if (err != NULL) {
			GRID_WARN("META0 request error (%d) : %s", err->code, err->message);
			if (CODE_IS_NETWORK_ERROR(err->code)) {
				exclude = g_slist_prepend(exclude,m0addr);
				m0addr = _getMeta0addr(&m0_lst,exclude);
			} else {
				m0addr = NULL;
			}
			g_clear_error(&err);
		} else {
			GRID_INFO("META0 Zookeeper node removed!");
			return;
		}
	}

	grid_main_set_status(1);
}

static void
meta0_action(void)
{
	if (flag_list) {
		meta0_init_list();
	}
	else if (flag_get) {
		meta0_init_get();
	}
	else if (flag_reload) {
		meta0_init_reload();
	}
	else if (flag_reset) {
		meta0_init_reset();
	}
	else if (flag_getmeta1info) {
		meta0_init_get_meta1_info();
	}
	else if (flag_destroy_meta1ref) {
		meta0_init_destroy_meta1ref();
	}
	else if (flag_destroy_zk_node) {
		meta0_init_destroy_zk_node();
	}
	else {
		GRID_INFO("No action specified");
	}
}

static const char *
meta0_usage(void)
{
	return "Namespace|IP:PORT (get PREFIX|list|reload|reset|get_meta1_info)";
}

static struct grid_main_option_s *
meta0_get_options(void)
{
	static struct grid_main_option_s meta0_options[] = {
		{"NoCheck", OT_BOOL, {.b=&flag_nocheck},
			"Disable checks to relaunch assign"},
		{NULL, OT_INT, {.i=NULL}, NULL}
	};
	return meta0_options;
}

static void
meta0_specific_fini(void)
{
}

static void
meta0_set_defaults(void)
{
	memset(&addr, 0, sizeof(addr));
}

static gboolean
meta0_configure(int argc, char **argv)
{
	const gchar *command;

	if (argc < 2) {
		GRID_WARN("Not enough options, see usage.");
		return FALSE;
	}

	if (!grid_string_to_addrinfo(argv[0], &addr))
		g_strlcpy(namespace, argv[0], sizeof(namespace));

	command = argv[1];
	if (!g_ascii_strcasecmp(command, "get")) {
		if (argc != 3) {
			GRID_WARN("Missing prefix for the get command, see usage.");
			return FALSE;
		}
		if (!oio_str_hex2bin(argv[2], prefix, 2)) {
			GRID_WARN("Invalid prefix for the get command, see usage.");
			return FALSE;
		}
		flag_get = TRUE;
		return TRUE;
	}
	if (!g_ascii_strcasecmp(command, "reload")) {
		if (argc > 2)
			GRID_DEBUG("Exceeding list arguments ignored.");
		flag_reload = TRUE;
		return TRUE;
	}
	if (!g_ascii_strcasecmp(command, "reset")) {
		if (argc > 2)
			GRID_DEBUG("Exceeding list arguments ignored.");
		flag_reset = TRUE;
		return TRUE;
	}
	if (!g_ascii_strcasecmp(command, "list")) {
		if (argc > 2)
			GRID_DEBUG("Exceeding list arguments ignored.");
		flag_list = TRUE;
		return TRUE;
	}
	if (!g_ascii_strcasecmp(command, "get_meta1_info")) {
		if (argc > 2)
			GRID_DEBUG("Exceeding list arguments ignored.");
		flag_getmeta1info = TRUE;
		return TRUE;
	}

	if (!g_ascii_strcasecmp(command, "destroy_meta1ref")) {
		if (argc != 3) {
			GRID_WARN("Missing META1 address");
			return FALSE;
		}
		if (!urlv_check(argv+2)) {
			GRID_WARN("Invalid META1 address");
			return FALSE;
		}
		urls = g_strdupv(argv+2);
		flag_destroy_meta1ref = TRUE;
		return TRUE;
	}

	if (!g_ascii_strcasecmp(command, "destroy_zknode")) {
		if (argc != 3) {
			GRID_WARN("Missing META1 address");
			return FALSE;
		}
		if (!urlv_check(argv+2)) {
			GRID_WARN("Invalid META1 address");
			return FALSE;
		}
		urls = g_strdupv(argv+2);
		flag_destroy_zk_node = TRUE;
		return TRUE;
	}

	GRID_WARN("Invalid command, see usage.");
	return FALSE;
}

static void
meta0_specific_stop(void)
{
	GRID_TRACE("STOP!");
}

static struct grid_main_callbacks meta0_callbacks =
{
	.options = meta0_get_options,
	.action = meta0_action,
	.set_defaults = meta0_set_defaults,
	.specific_fini = meta0_specific_fini,
	.configure = meta0_configure,
	.usage = meta0_usage,
	.specific_stop = meta0_specific_stop,
};

int
main(int argc, char ** argv)
{
	return grid_main_cli (argc, argv, &meta0_callbacks);
}

