/*
OpenIO SDS sqlx
Copyright (C) 2014 Worldine, original work as part of Redcurrant
Copyright (C) 2015-2016 OpenIO, as part of OpenIO Software Defined Storage

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

#include <stdlib.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <malloc.h>
#include <sys/resource.h>

#include <metautils/lib/metautils.h>
#include <metautils/lib/server_variables.h>

#include <cluster/lib/gridcluster.h>
#include <events/oio_events_queue.h>
#include <events/oio_events_queue_zmq.h>
#include <events/oio_events_queue_beanstalkd.h>
#include <server/network_server.h>
#include <server/internals.h>
#include <server/transport_gridd.h>
#include <sqliterepo/sqlx_macros.h>
#include <sqliterepo/sqliterepo.h>
#include <sqliterepo/cache.h>
#include <sqliterepo/election.h>
#include <sqliterepo/synchro.h>
#include <sqliterepo/replication_dispatcher.h>
#include <sqliterepo/gridd_client_pool.h>
#include <sqliterepo/internals.h>
#include <resolver/hc_resolver.h>

#include <core/oiolb.h>

#include <glib.h>

#include "sqlx_service.h"

// common_main hooks
static struct grid_main_option_s * sqlx_service_get_options(void);
static const char * sqlx_service_usage(void);
static gboolean sqlx_service_configure(int argc, char **argv);
static void sqlx_service_action(void);
static void sqlx_service_set_defaults(void);
static void sqlx_service_specific_fini(void);
static void sqlx_service_specific_stop(void);

// Periodic tasks & thread's workers
static void _task_malloc_trim(gpointer p);
static void _task_expire_bases(gpointer p);
static void _task_expire_resolver(gpointer p);
static void _task_react_elections(gpointer p);
static void _task_reload_nsinfo(gpointer p);
static void _task_reload_workers(gpointer p);

static gpointer _worker_queue (gpointer p);
static gpointer _worker_clients (gpointer p);

static const struct gridd_request_descr_s * _get_service_requests (void);

static GError* _reload_lb_world(
		struct oio_lb_world_s *lbw, struct oio_lb_s *lb);

// Static variables
static struct sqlx_service_s SRV = {{0}};
static struct replication_config_s replication_config = {0};
static struct grid_main_callbacks sqlx_service_callbacks =
{
	.options = sqlx_service_get_options,
	.action = sqlx_service_action,
	.set_defaults = sqlx_service_set_defaults,
	.specific_fini = sqlx_service_specific_fini,
	.configure = sqlx_service_configure,
	.usage = sqlx_service_usage,
	.specific_stop = sqlx_service_specific_stop,
};

static struct grid_main_option_s *custom_options = NULL;
static struct grid_main_option_s *all_options = NULL;
static struct grid_main_option_s common_options[] =
{
	{"Endpoint", OT_STRING, {.str = &SRV.url},
		"Bind to this IP:PORT couple instead of 0.0.0.0 and a random port"},
	{"Announce", OT_STRING, {.str = &SRV.announce},
		"Announce this IP:PORT couple instead of the TCP endpoint"},

	{"SysConfig", OT_BOOL, {.b = &SRV.config_system},
		"Load the system configuration and overload the central variables"},
	{"Config", OT_LIST, {.lst = &SRV.config_paths},
		"Load the given file and overload the central variables"},

	{"Replicate", OT_BOOL, {.b = &SRV.flag_replicable},
		"DO NOT USE THIS. This might disable the replication"},

	{"Sqlx.Sync.Repli", OT_UINT, {.u = &SRV.sync_mode_repli},
		"SYNC mode to be applied on replicated bases after open "
			"(0=NONE,1=NORMAL,2=FULL)"},
	{"Sqlx.Sync.Solo", OT_UINT, {.u = &SRV.sync_mode_solo},
		"SYNC mode to be applied on non-replicated bases after open "
			"(0=NONE,1=NORMAL,2=FULL)"},

	{"MaxBasesHard", OT_UINT, {.u = &SRV.cfg_max_bases_hard},
		"Absolute max number of cached bases. Won't ever be increased." },
	{"MaxBases", OT_UINT, {.u = &SRV.cfg_max_bases_soft},
		"Limits the number of concurrent open bases (0=automatic), bounded "
			"to MaxBasesHard naturally"	},

	{"MaxPassive", OT_UINT, {.u = &SRV.cfg_max_passive},
		"Limits the number of concurrent passive connections (0=automatic)" },
	{"MaxActive", OT_UINT, {.u = &SRV.cfg_max_active},
		"Limits the number of concurrent active connections (0=automatic)" },
	{"MaxWorkers", OT_UINT, {.u=&SRV.cfg_max_workers},
		"Limits the number of worker threads" },

	{"CacheEnabled", OT_BOOL, {.b = &SRV.flag_cached_bases},
		"If set, each base will be cached in a way it won't be accessed"
			" by several requests in the same time."},
	{"DeleteEnabled", OT_BOOL, {.b = &SRV.flag_delete_on},
		"If not set, prevents deleting database files from disk"},

	{NULL, 0, {.i=0}, NULL}
};

// repository hooks ------------------------------------------------------------

static const gchar *
_get_url(gpointer ctx)
{
	EXTRA_ASSERT(ctx != NULL);
	return PSRV(ctx)->announce->str;
}

static GError*
_get_version(gpointer ctx, const struct sqlx_name_s *n, GTree **result)
{
	EXTRA_ASSERT(ctx != NULL);
	return sqlx_repository_get_version2(PSRV(ctx)->repository, n, result);
}

// sqlite_service configuration steps ------------------------------------------

static gboolean
_configure_with_arguments(struct sqlx_service_s *ss, int argc, char **argv)
{
	// Sanity checks
	if (ss->sync_mode_solo > SQLX_SYNC_FULL) {
		GRID_WARN("Invalid SYNC mode for not-replicated bases");
		return FALSE;
	}
	if (ss->sync_mode_repli > SQLX_SYNC_FULL) {
		GRID_WARN("Invalid SYNC mode for replicated bases");
		return FALSE;
	}
	if (!ss->url) {
		GRID_WARN("No URL!");
		return FALSE;
	}
	if (!ss->announce) {
		ss->announce = g_string_new(ss->url->str);
		GRID_DEBUG("No announce set, using endpoint [%s]", ss->announce->str);
	}
	if (!metautils_url_valid_for_bind(ss->url->str)) {
		GRID_ERROR("Invalid URL as a endpoint [%s]", ss->url->str);
		return FALSE;
	}
	if (!metautils_url_valid_for_connect(ss->announce->str)) {
		GRID_ERROR("Invalid URL to be announced [%s]", ss->announce->str);
		return FALSE;
	}

	/* Positional argument: NS */

	if (argc < 2) {
		GRID_ERROR("Not enough options, see usage.");
		return FALSE;
	}
	gsize s = g_strlcpy(ss->ns_name, argv[0], sizeof(ss->ns_name));
	if (s >= sizeof(ss->ns_name)) {
		GRID_WARN("Namespace name too long (given=%"G_GSIZE_FORMAT" max=%u)",
				s, (unsigned int)sizeof(ss->ns_name));
		return FALSE;
	}
	GRID_DEBUG("NS configured to [%s]", ss->ns_name);

	/* Positional argument: VOLUME */

	s = g_strlcpy(ss->volume, argv[1], sizeof(ss->volume));
	if (s >= sizeof(ss->volume)) {
		GRID_WARN("Volume name too long (given=%"G_GSIZE_FORMAT" max=%u)",
				s, (unsigned int) sizeof(ss->volume));
		return FALSE;
	}
	GRID_DEBUG("Volume configured to [%s]", ss->volume);

	/* Before loading what remains, first populate the central configuration
	 * facility. This is a pure side effect but the value might have an impact
	 * even on the structure creations. */

	/* TODO(jfs): deduplicate this with its copy in proxy/metacd_http.c */
	struct oio_cfg_handle_s *ns_conf = oio_cfg_cache_create();
	oio_cfg_set_handle(ns_conf);
	if (SRV.config_system)
		oio_var_value_all_with_config(ns_conf, ss->ns_name);
	ns_conf = NULL; /* value now global */

	for (GSList *l = SRV.config_paths; l ; l = l->next) {
		if (!l->data)
			continue;
		struct oio_cfg_handle_s *cfg =
			oio_cfg_cache_create_fragment(l->data);
		oio_var_value_all_with_config(cfg, ss->ns_name);
		oio_cfg_handle_clean(cfg);
	}

	/* Load the ZK url.
	 * We first look for an URL common to all the services, and we will maybe
	 * override it with a value specific for the file. */
	ss->zk_url = oio_cfg_get_value(ss->ns_name, OIO_CFG_ZOOKEEPER);

	do {
		gchar k[sizeof(OIO_CFG_ZOOKEEPER)+2+LIMIT_LENGTH_SRVTYPE];
		g_snprintf(k, sizeof(k), "%s.%s", OIO_CFG_ZOOKEEPER, ss->service_config->srvtype);
		gchar *str = oio_cfg_get_value(ss->ns_name, k);
		if (str)
			GRID_NOTICE("ZK [%s] <- [%s] (at %s)", ss->zk_url, str, k);
		else
			GRID_DEBUG("ZK [%s] (nothing at %s)", ss->zk_url, k);
		if (str) oio_str_reuse(&ss->zk_url, str);
	} while (0);

	GRID_NOTICE("UDP %s", oio_udp_allowed ? "allowed" : "forbidden");

	/* Check if TCP_FASTOPEN is allowed */
	GRID_NOTICE("TCP_FASTOPEN %s", oio_socket_fastopen ? "allowed" : "forbidden");

	if (oio_log_outgoing)
		GRID_NOTICE("Outgoing requests log [ON]");

	return TRUE;
}

static gboolean
_configure_limits(struct sqlx_service_s *ss)
{
	guint newval = 0, max = 0, total = 0, available = 0, min = 0;
	struct rlimit limit = {0, 0};
#define CONFIGURE_LIMIT(cfg,real) do { \
	max = MIN(max, available); \
	newval = (cfg > 0 && cfg < max) ? cfg : max; \
	newval = newval > min? newval : min; \
	real = newval; \
	available -= real; \
} while (0)

	if (0 != getrlimit(RLIMIT_NOFILE, &limit)) {
		GRID_ERROR("Max file descriptor unknown: getrlimit error "
				"(errno=%d) %s", errno, strerror(errno));
		return FALSE;
	}
	if (limit.rlim_cur < 64) {
		GRID_ERROR("Not enough file descriptors allowed [%lu], "
				"minimum 64 required", (unsigned long) limit.rlim_cur);
		return FALSE;
	}

	// We keep 20 FDs for unexpected cases (sqlite sometimes uses
	// temporary files, even when we ask for memory journals).
	total = (limit.rlim_cur - 20);
	// If user sets outstanding values for the first 2 parameters,
	// there is still 2% available for the 3rd.
	max = total * 49 / 100;
	// Hardcoded in sqlx_repository_configure_maxbases()
	min = 4;

	available = total;

	/* max_bases cannot be changed at runtime, so we set it first and
	 * clamp the other limits accordingly.
	 * For backward compatibility purposes, we configure the SOFT limit
	 * instead of the hard one, when only "MaxBases" is set, and we deduce
	 * the HARD limit. This is the limit the users will see in action, but
	 * with a room for expansion though.
	 */
	CONFIGURE_LIMIT(
			(ss->cfg_max_bases_soft > 0?
				ss->cfg_max_bases_soft : SQLX_MAX_BASES_PERCENT(total)),
			ss->max_bases_soft);
	ss->max_bases_hard = ss->cfg_max_bases_hard > 0 ?
				ss->cfg_max_bases_hard : sqliterepo_repo_max_bases;

	// max_passive > max_active permits answering to clients while
	// managing internal procedures (elections, replications...).
	CONFIGURE_LIMIT(
			(ss->cfg_max_passive > 0?
				ss->cfg_max_passive : SQLX_MAX_PASSIVE_PERCENT(total)),
			ss->max_passive);
	CONFIGURE_LIMIT(
			(ss->cfg_max_active > 0?
				ss->cfg_max_active : SQLX_MAX_ACTIVE_PERCENT(total)),
			ss->max_active);

	GRID_INFO("Limits set to ACTIVES[%u] PASSIVES[%u] BASES[%u/%u] "
			"fd=%u/%u",
			ss->max_active, ss->max_passive,
			ss->max_bases_soft, ss->max_bases_hard,
			ss->max_active + ss->max_passive + ss->max_bases_soft,
			(guint)limit.rlim_cur);

	return TRUE;
#undef CONFIGURE_LIMIT
}

static gboolean
_init_configless_structures(struct sqlx_service_s *ss)
{
	if (!(ss->lb_world = oio_lb_local__create_world())
			|| !(ss->lb = oio_lb__create())
			|| !(ss->server = network_server_init())
			|| !(ss->dispatcher = transport_gridd_build_empty_dispatcher())
			|| !(ss->clients_pool = gridd_client_pool_create())
			|| !(ss->clients_factory = gridd_client_factory_create())
			|| !(ss->resolver = hc_resolver_create())
			|| !(ss->gtq_admin = grid_task_queue_create("admin"))
			|| !(ss->gtq_reload = grid_task_queue_create("reload"))) {
		GRID_WARN("SERVICE init error: memory allocation failure");
		return FALSE;
	}

	return TRUE;
}

static gboolean
_configure_peering (struct sqlx_service_s *ss)
{
	ss->peering = sqlx_peering_factory__create_direct
		(ss->clients_pool, ss->clients_factory);
	return TRUE;
}

static gboolean
_configure_synchronism(struct sqlx_service_s *ss)
{
	if (!ss->zk_url) {
		GRID_NOTICE("SYNC off (no zookeeper)");
		return TRUE;
	}

	ss->sync = sqlx_sync_create(ss->zk_url);
	if (!ss->sync)
		return FALSE;

	gchar *realprefix = g_strdup_printf("/hc/ns/%s/%s", ss->ns_name,
			ss->service_config->zk_prefix);
	sqlx_sync_set_prefix(ss->sync, realprefix);
	g_free(realprefix);

	sqlx_sync_set_hash(ss->sync, ss->service_config->zk_hash_width,
			ss->service_config->zk_hash_depth);

	GError *err = sqlx_sync_open(ss->sync);
	if (err != NULL) {
		GRID_WARN("SYNC init error: (%d) %s", err->code, err->message);
		g_clear_error(&err);
		return FALSE;
	}

	return TRUE;
}

static gchar **
filter_services(struct sqlx_service_s *ss, gchar **s, const gchar *type)
{
	gboolean matched = FALSE;
	GPtrArray *tmp = g_ptr_array_new();
	for (; *s ;s++) {
		struct meta1_service_url_s *u = meta1_unpack_url(*s);
		gboolean srvtype_matched;
		gchar *host;
		if (u) {
			srvtype_matched = !strcmp(type, u->srvtype);
			host = u->host;
		} else {
			srvtype_matched = TRUE;
			host = *s;
		}
		if (srvtype_matched) {
			if (!g_ascii_strcasecmp(host, ss->url->str))
				matched = TRUE;
			else
				g_ptr_array_add(tmp, g_strdup(host));
		}
		meta1_service_url_clean(u);
	}

	gchar **out = (gchar**)metautils_gpa_to_array (tmp, TRUE);
	if (matched)
		return out;
	g_strfreev (out);
	return NULL;
}

static gchar **
filter_services_and_clean(struct sqlx_service_s *ss,
		gchar **src, const gchar *type)
{
	if (!src)
		return NULL;
	gchar **result = filter_services(ss, src, type);
	g_strfreev(src);
	return result;
}

GError *
sqlx_service_resolve_peers(struct sqlx_service_s *ss,
		const struct sqlx_name_s *n, gboolean nocache, gchar ***result)
{
	EXTRA_ASSERT(ss != NULL);
	EXTRA_ASSERT(result != NULL);

	gint retry = TRUE;
	GError *err = NULL;

	gint64 seq = 1;
	struct oio_url_s *u = oio_url_empty ();
	oio_url_set(u, OIOURL_NS, ss->ns_name);
	if (!sqlx_name_extract(n, u, ss->service_config->srvtype, &seq)) {
		oio_url_pclean (&u);
		return BADREQ("Invalid type name: '%s'", n->type);
	}

label_retry:
	if (nocache) {
		hc_decache_reference_service(ss->resolver, u, n->type);
	}

	err = hc_resolve_reference_service(ss->resolver, u, n->type, result);

	if (err) {
		if (retry && err->code == CODE_RANGE_NOTFOUND) {
			// We may have asked the wrong meta1
			hc_decache_reference(ss->resolver, u);
			retry = FALSE;
			goto label_retry;
		}
		g_prefix_error(&err, "Peer resolution error: ");
	}

	oio_url_clean(u);
	return err;
}

// TODO: replace `nocache` by flags
static GError *
_get_peers_wrapper(struct sqlx_service_s *ss, const struct sqlx_name_s *name,
		gboolean nocache, gchar ***result)
{
	EXTRA_ASSERT(ss != NULL);
	EXTRA_ASSERT(result != NULL);

	gchar **peers = NULL;
	GError *err = NULL;

	// Try to read peers from the database
	if (!nocache) {
		err = sqlx_repository_get_peers2(ss->repository, name, &peers);
		if (err) {
			GRID_INFO("Failed to get_peers() from local database: (%d) %s",
					err->code, err->message);
			g_clear_error(&err);
		}
	}

label_retry:
	// Try to read peers from the upper-level service
	if (!peers) {
		err = ss->service_config->get_peers(ss, name, nocache, &peers);
	}

	if (!err) {
		*result = filter_services_and_clean(ss, peers, name->type);
		if (!*result) {
			// If cache was enabled, we can retry without cache
			if (!nocache) {
				nocache = TRUE;
				goto label_retry;
			} else {
				err = NEWERROR(CODE_CONTAINER_NOTFOUND, "Base not managed");
			}
		}
	} else if (err->code == CODE_RANGE_NOTFOUND && !nocache) {
		// We may have asked the wrong upper-level service
		// TODO: call hc_decache_reference(ss->resolver, url);
		// and remove the retry loop from meta2
		nocache = TRUE;
		goto label_retry;
	}

	return err;
}

static gboolean
_configure_replication(struct sqlx_service_s *ss)
{
	GRID_DEBUG("Got zookeeper URL [%s]", ss->zk_url);
	replication_config.mode = (ss->flag_replicable && ss->zk_url != NULL)
		? ELECTION_MODE_QUORUM : ELECTION_MODE_NONE;
	replication_config.ctx = ss;
	replication_config.get_local_url = _get_url;
	replication_config.get_version = _get_version;
	replication_config.get_peers = (GError* (*)(gpointer,
			const struct sqlx_name_s*, gboolean, gchar ***))
			_get_peers_wrapper;

	GError *err = election_manager_create(&replication_config,
			&ss->election_manager);
	if (err != NULL) {
		GRID_WARN("Replication init failure: (%d) %s",
				err->code, err->message);
		g_clear_error(&err);
		return FALSE;
	}

	election_manager_dump_delays();
	return TRUE;
}

static gboolean
_configure_backend(struct sqlx_service_s *ss)
{
	struct sqlx_repo_config_s repository_config = {0};
	repository_config.flags = 0;
	repository_config.flags |= ss->flag_delete_on ? SQLX_REPO_DELETEON : 0;
	repository_config.flags |= ss->flag_cached_bases ? 0 : SQLX_REPO_NOCACHE;
	repository_config.flags |= ss->flag_autocreate ? SQLX_REPO_AUTOCREATE : 0;
	repository_config.sync_solo = ss->sync_mode_solo;
	repository_config.sync_repli = ss->sync_mode_repli;

	repository_config.max_bases = ss->max_bases_hard;

	GError *err = sqlx_repository_init(ss->volume, &repository_config,
			&ss->repository);
	if (err) {
		GRID_ERROR("SQLX repository init failure : (%d) %s",
				err->code, err->message);
		g_clear_error(&err);
		return FALSE;
	}

	sqlx_repository_configure_maxbases(SRV.repository, SRV.max_bases_soft);

	err = sqlx_repository_configure_type(ss->repository,
			ss->service_config->srvtype, ss->service_config->schema);

	if (err) {
		GRID_ERROR("SQLX schema init failure : (%d) %s", err->code, err->message);
		g_clear_error(&err);
		return FALSE;
	}

	sqlx_repository_configure_hash (ss->repository,
			ss->service_config->repo_hash_width,
			ss->service_config->repo_hash_depth);

	GRID_TRACE("SQLX repository initiated");
	return TRUE;
}

static gboolean
_configure_tasks(struct sqlx_service_s *ss)
{
	grid_task_queue_register(ss->gtq_reload, 5, _task_reload_nsinfo, NULL, ss);
	grid_task_queue_register(ss->gtq_reload, 5, _task_reload_workers, NULL, ss);

	grid_task_queue_register(ss->gtq_admin, 1, _task_expire_bases, NULL, ss);
	grid_task_queue_register(ss->gtq_admin, 1, _task_expire_resolver, NULL, ss);
	grid_task_queue_register(ss->gtq_admin, 1, _task_react_elections, NULL, ss);
	grid_task_queue_register(ss->gtq_admin, 3600, _task_malloc_trim, NULL, ss);

	return TRUE;
}

static gboolean
_configure_events_queue (struct sqlx_service_s *ss)
{
	if (ss->flag_no_event) {
		GRID_DEBUG("Events queue disabled, the service disabled it");
		return TRUE;
	}

	gchar *url =  oio_cfg_get_eventagent (SRV.ns_name);
	STRING_STACKIFY (url);

	if (!url) {
		GRID_DEBUG("Events queue disabled, no URL configured");
		return TRUE;
	}

	GError *err = oio_events_queue_factory__create(url, &ss->events_queue);

	if (!ss->events_queue) {
		GRID_WARN("Events queue creation failure: (%d) %s", err->code, err->message);
		return FALSE;
	}

	GRID_INFO("Event queue ready, connected to [%s]", url);
	return TRUE;
}

static gboolean
_configure_network(struct sqlx_service_s *ss)
{
	transport_gridd_dispatcher_add_requests(ss->dispatcher,
			sqlx_repli_gridd_get_requests(), ss->repository);
	transport_gridd_dispatcher_add_requests(ss->dispatcher,
			_get_service_requests(), ss);
	return TRUE;
}

// common_main hooks -----------------------------------------------------------

static void
_action_report_error(GError *err, const gchar *msg)
{
	GRID_ERROR("%s : (%d) %s", msg, !err?0:err->code, !err?"":err->message);
	if (err)
		g_clear_error(&err);
	grid_main_stop();
	return;
}

static void
sqlx_service_action(void)
{
	GError *err = NULL;

	if (oio_client_cache_errors)
		GRID_NOTICE("Faulty peers avoidance: ENABLED");

	if (!SRV.flag_nolock) {
		err = volume_service_lock (SRV.volume, SRV.service_config->srvtype,
				SRV.announce->str, SRV.ns_name);
		if (err)
			return _action_report_error(err, "Volume lock failed");
	}

	oio_server_volume = SRV.volume;

	gridd_client_pool_set_max(SRV.clients_pool, SRV.max_active);
	network_server_set_maxcnx(SRV.server, SRV.max_passive);

	election_manager_set_peering(SRV.election_manager, SRV.peering);
	if (SRV.sync)
		election_manager_set_sync(SRV.election_manager, SRV.sync);
	sqlx_repository_set_elections(SRV.repository, SRV.election_manager);

	grid_task_queue_fire(SRV.gtq_reload);
	grid_task_queue_fire(SRV.gtq_admin);
	GRID_DEBUG("All tasks now fired once");

	/* Start the administrative threads */
	SRV.thread_admin = grid_task_queue_run(SRV.gtq_admin, &err);
	if (!SRV.thread_admin)
		return _action_report_error(err, "Failed to start the ADMIN thread");

	SRV.thread_reload = grid_task_queue_run(SRV.gtq_reload, &err);
	if (!SRV.thread_reload)
		return _action_report_error(err, "Failed to start the RELOAD thread");

	SRV.thread_client = g_thread_try_new("clients", _worker_clients, &SRV, &err);
	if (!SRV.thread_client)
		return _action_report_error(err, "Failed to start the CLIENT thread");

	if (SRV.events_queue) {
		SRV.thread_queue = g_thread_try_new("queue", _worker_queue, &SRV, &err);
		if (!SRV.thread_queue)
			return _action_report_error(err, "Failed to start the QUEUE thread");
	}

	/* open all the sockets */
	if (!grid_main_is_running())
		return;
	network_server_allow_udp(SRV.server);
	grid_daemon_bind_host(SRV.server, SRV.url->str, SRV.dispatcher);
	err = network_server_open_servers(SRV.server);
	if (NULL != err)
		return _action_report_error(err, "GRIDD bind failure");
	if (!grid_main_is_running())
		return;

	if (oio_udp_allowed) {
		int fd_udp = network_server_first_udp(SRV.server);
		GRID_DEBUG("UDP socket fd=%d", fd_udp);
		sqlx_peering_direct__set_udp(SRV.peering, fd_udp);
	}

	/* SERVER/GRIDD main run loop */
	if (NULL != (err = network_server_run(SRV.server)))
		return _action_report_error(err, "GRIDD run failure");
}

static void
sqlx_service_specific_stop(void)
{
	if (SRV.server)
		network_server_stop(SRV.server);

	if (SRV.gtq_admin)
		grid_task_queue_stop(SRV.gtq_admin);
	if (SRV.gtq_reload)
		grid_task_queue_stop(SRV.gtq_reload);
}

static gboolean
sqlx_service_configure(int argc, char **argv)
{
	return _configure_limits(&SRV)
		&& _init_configless_structures(&SRV)
		&& _configure_with_arguments(&SRV, argc, argv)
		/* NS now known! */
		&& _configure_synchronism(&SRV)
		&& _configure_peering(&SRV)
		&& _configure_replication(&SRV)
		&& _configure_backend(&SRV)
		&& _configure_tasks(&SRV)
		&& _configure_network(&SRV)
		&& _configure_events_queue(&SRV)
		&& (!SRV.service_config->post_config
				|| SRV.service_config->post_config(&SRV));
}

static void
sqlx_service_set_defaults(void)
{
	SRV.cfg_max_bases_soft = 0;
	SRV.cfg_max_bases_hard = sqliterepo_repo_max_bases;
	SRV.cfg_max_passive = 0;
	SRV.cfg_max_active = 0;
	SRV.cfg_max_workers = 200;
	SRV.flag_replicable = TRUE;
	SRV.flag_autocreate = TRUE;
	SRV.flag_delete_on = TRUE;
	SRV.flag_cached_bases = TRUE;

	SRV.sync_mode_solo = 1;
	SRV.sync_mode_repli = 0;

	if (SRV.service_config->set_defaults)
		SRV.service_config->set_defaults(&SRV);
}

static void
sqlx_service_specific_fini(void)
{
	// soft stop
	if (SRV.gtq_reload)
		grid_task_queue_stop(SRV.gtq_reload);
	if (SRV.gtq_admin)
		grid_task_queue_stop(SRV.gtq_admin);

	if (SRV.server) {
		network_server_close_servers(SRV.server);
	}

	if (SRV.thread_reload)
		g_thread_join(SRV.thread_reload);
	if (SRV.thread_admin)
		g_thread_join(SRV.thread_admin);
	if (SRV.thread_client)
		g_thread_join(SRV.thread_client);
	if (SRV.thread_queue)
		g_thread_join(SRV.thread_queue);

	if (SRV.repository) {
		sqlx_repository_stop(SRV.repository);
		struct sqlx_cache_s *cache = sqlx_repository_get_cache(SRV.repository);
		if (cache)
			sqlx_cache_expire(cache, G_MAXUINT, 0);
	}
	if (election_manager_is_operational(SRV.election_manager))
		election_manager_exit_all(SRV.election_manager,
				sqliterepo_server_exit_ttl, TRUE);
	if (SRV.sync)
		sqlx_sync_close(SRV.sync);
	if (SRV.peering)
		sqlx_peering__destroy(SRV.peering);

	// Stop the server AFTER cleaning all elections
	if (SRV.server)
		network_server_stop(SRV.server);

	// Cleanup
	if (SRV.gtq_admin) {
		grid_task_queue_destroy(SRV.gtq_admin);
		SRV.gtq_admin = NULL;
	}
	if (SRV.gtq_reload) {
		grid_task_queue_destroy(SRV.gtq_reload);
		SRV.gtq_reload = NULL;
	}

	if (SRV.server) {
		network_server_clean(SRV.server);
		SRV.server = NULL;
	}
	if (SRV.dispatcher) {
		gridd_request_dispatcher_clean(SRV.dispatcher);
		SRV.dispatcher = NULL;
	}
	if (SRV.repository) {
		sqlx_repository_clean(SRV.repository);
		SRV.repository = NULL;
	}
	if (SRV.resolver) {
		hc_resolver_destroy(SRV.resolver);
		SRV.resolver = NULL;
	}

	if (SRV.announce) {
		g_string_free(SRV.announce, TRUE);
		SRV.announce = NULL;
	}
	if (SRV.url) {
		g_string_free(SRV.url, TRUE);
		SRV.url = NULL;
	}
	if (SRV.zk_url)
		oio_str_clean(&SRV.zk_url);

	if (SRV.clients_pool) {
		gridd_client_pool_destroy (SRV.clients_pool);
		SRV.clients_pool = NULL;
	}

	if (SRV.clients_factory) {
		gridd_client_factory_clean(SRV.clients_factory);
		SRV.clients_factory = NULL;
	}

	// Must be freed after SRV.clients_pool
	if (SRV.election_manager) {
		election_manager_clean(SRV.election_manager);
		SRV.election_manager = NULL;
	}
	if (SRV.sync) {
		sqlx_sync_clear(SRV.sync);
		SRV.sync = NULL;
	}

	if (SRV.lb)
		oio_lb__clear(&SRV.lb);

	if (SRV.lb_world) {
		oio_lb_world__destroy(SRV.lb_world);
		SRV.lb_world = NULL;
	}

	if (SRV.events_queue) {
		oio_events_queue__destroy (SRV.events_queue);
		SRV.events_queue = NULL;
	}

	if (SRV.nsinfo) {
		namespace_info_free(SRV.nsinfo);
		SRV.nsinfo = NULL;
	}
	if (all_options) {
		g_free (all_options);
		all_options = NULL;
	}

	oio_cfg_set_handle(NULL);
}

static guint
_count_options (struct grid_main_option_s const * tab)
{
	guint count = 0;
	if (tab)
		for (; tab->name; ++tab, ++count)
			;
	return count;
}

void
sqlx_service_set_custom_options (struct grid_main_option_s *tab)
{
	custom_options = tab;
}

static struct grid_main_option_s *
sqlx_service_get_options(void)
{
	if (!all_options) {
		const guint count_common = _count_options (common_options);
		const guint count_custom = _count_options (custom_options);
		const guint count_total = count_custom + count_common;
		all_options = g_malloc0 (
				(1+count_total) * sizeof(struct grid_main_option_s));
		if (count_common)
			memcpy (all_options, common_options,
					count_common * sizeof(struct grid_main_option_s));
		if (count_custom)
			memcpy (all_options + count_common, custom_options,
					count_custom * sizeof(struct grid_main_option_s));
	}
	return all_options;
}

static const char *
sqlx_service_usage(void)
{
	return "NS VOLUME";
}

int
sqlite_service_main(int argc, char **argv,
		const struct sqlx_service_config_s *cfg)
{
	SRV.replication_config = &replication_config;
	SRV.service_config = cfg;
	int rc = grid_main(argc, argv, &sqlx_service_callbacks);
	return rc;
}

/* Tasks -------------------------------------------------------------------- */

static gboolean
_event_running (gboolean pending)
{
	(void) pending;
	return grid_main_is_running ();
}

static gpointer
_worker_queue (gpointer p)
{
	metautils_ignore_signals();
	struct sqlx_service_s *ss = PSRV(p);
	if (ss && ss->events_queue)
		oio_events_queue__run (ss->events_queue, _event_running);
	return p;
}

static gpointer
_worker_clients(gpointer p)
{
	metautils_ignore_signals();
	while (grid_main_is_running()) {
		GError *err = gridd_client_pool_round(PSRV(p)->clients_pool, 1);
		if (err != NULL) {
			GRID_ERROR("Clients error : (%d) %s", err->code, err->message);
			g_clear_error(&err);
			grid_main_stop();
		}
	}
	return p;
}

static void
_task_malloc_trim(gpointer p)
{
	(void) p;
	malloc_trim (malloc_trim_size_periodic);
}

static void
_task_expire_bases(gpointer p)
{
	if (!grid_main_is_running ())
		return;

	struct sqlx_cache_s *cache = sqlx_repository_get_cache(PSRV(p)->repository);
	if (cache != NULL) {
		guint count = sqlx_cache_expire(cache, 100, 500 * G_TIME_SPAN_MILLISECOND);
		if (count)
			GRID_DEBUG("Expired %u bases", count);
	}
}

static void
_task_expire_resolver(gpointer p)
{
	if (!grid_main_is_running ())
		return;

	guint count = hc_resolver_expire(PSRV(p)->resolver);
	if (count)
		GRID_DEBUG("Expired %u entries from the resolver cache", count);
}

static void
_task_react_elections(gpointer p)
{
	if (!grid_main_is_running ())
		return;
	if (!PSRV(p)->flag_replicable)
		return;

	gint64 t = oio_ext_monotonic_time();
	guint count = election_manager_play_timers (PSRV(p)->election_manager,
			sqliterepo_election_expire_max_per_round);
	t = t - oio_ext_monotonic_time();

	if (count || t > (500*G_TIME_SPAN_MILLISECOND)) {
		GRID_DEBUG("Reacted %u elections in %"G_GINT64_FORMAT"ms",
				count, t / G_TIME_SPAN_MILLISECOND);
	}
}

static void
_task_reload_nsinfo(gpointer p)
{
	if (!grid_main_is_running ())
		return;

	struct namespace_info_s *ni = NULL, *old = NULL;
	GError *err = conscience_get_namespace(PSRV(p)->ns_name, &ni);
	EXTRA_ASSERT ((err != NULL) ^ (ni != NULL));

	if (err) {
		GRID_WARN("NSINFO reload error [%s]: (%d) %s",
				PSRV(p)->ns_name, err->code, err->message);
		g_clear_error(&err);
	} else {
		old = PSRV(p)->nsinfo;
		PSRV(p)->nsinfo = ni;
		namespace_info_free(old);
	}
}

static void
_task_reload_workers(gpointer p)
{
	if (!grid_main_is_running ())
		return;
	if (!PSRV(p)->nsinfo)
		return;

	gint64 max_workers = namespace_info_get_srv_param_i64 (PSRV(p)->nsinfo,
			NULL, PSRV(p)->service_config->srvtype, "max_workers",
			SRV.cfg_max_workers);
	network_server_set_max_workers(PSRV(p)->server, (guint) max_workers);
}

static void
_reload_lb_service_types(struct oio_lb_world_s *lbw, struct oio_lb_s *lb,
		gchar **srvtypes, GPtrArray *tabsrv, GPtrArray *taberr)
{
	struct service_update_policies_s *pols = service_update_policies_create();
	gchar *pols_cfg = gridcluster_get_service_update_policy(SRV.nsinfo);
	service_update_reconfigure(pols, pols_cfg);
	g_free(pols_cfg);

	for (guint i=0; srvtypes[i] ;++i) {
		const char * srvtype = srvtypes[i];
		if (!oio_lb__has_pool(lb, srvtype)) {
			GRID_DEBUG("Creating pool for service type [%s]", srvtype);
			oio_lb__force_pool(lb,
					oio_lb_pool__from_service_policy( lbw, srvtype, pols));
		}

		if (!taberr->pdata[i])
			oio_lb_world__feed_service_info_list(lbw, tabsrv->pdata[i]);
	}

	service_update_policies_destroy(pols);
}

static void
_free_list_of_services(gpointer p)
{
	if (!p)
		return;
	g_slist_free_full((GSList*)p, (GDestroyNotify)service_info_clean);
}

static void
_free_error(gpointer p)
{
	if (!p)
		return;
	g_error_free((GError*)p);
}

static GError*
_reload_lb_world(struct oio_lb_world_s *lbw, struct oio_lb_s *lb)
{
	gchar **srvtypes = NULL;
	GPtrArray *tabsrv = NULL, *taberr = NULL;
	gboolean any_loading_error = FALSE;

	/* Load the list of service types */
	if (SRV.srvtypes[0] && SRV.srvtypes[0] != '!') {
		srvtypes = g_strsplit(SRV.srvtypes, ",", -1);
	} else {
		GSList *list_srvtypes = NULL;
		GError *err = conscience_get_types(SRV.ns_name, &list_srvtypes);
		if (err) {
			g_prefix_error(&err, "LB pool reload error: ");
			return err;
		}
		if (!SRV.srvtypes[0])
			srvtypes = (gchar**) metautils_list_to_array(list_srvtypes);
		else {
			/* the application gives an exclusion list */
			EXTRA_ASSERT(SRV.srvtypes[0] == '!');
			srvtypes = g_malloc0(sizeof(void*) *
					(1 + g_slist_length(list_srvtypes)));
			guint i = 0;
			for (GSList *l=list_srvtypes; l ;l=l->next) {
				const char * srvtype = l->data;
				if (!g_strstr_len(SRV.srvtypes+1, -1, srvtype)) {
					/* service type not excluded */
					srvtypes[i++] = l->data;
				} else {
					g_free(l->data);
				}
			}
		}
		g_slist_free(list_srvtypes);
	}

	EXTRA_ASSERT(srvtypes != NULL);
	if (!*srvtypes) {
		g_strfreev(srvtypes);
		return NULL;
	}

	/* Now preload all the service of these types */
	tabsrv = g_ptr_array_new_full(8, _free_list_of_services);
	taberr = g_ptr_array_new_full(8, _free_error);
	for (char **pst=srvtypes; *pst ;++pst) {
		const char * srvtype = *pst;
		GSList *srv = NULL;
		GError *e = conscience_get_services(SRV.ns_name, srvtype, FALSE, &srv);
		if (e) {
			GRID_WARN("Failed to load the list of [%s] in NS=%s", SRV.ns_name, srvtype);
			any_loading_error = TRUE;
		}
		g_ptr_array_add(tabsrv, srv);
		g_ptr_array_add(taberr, e);
	}

	/* Now refresh the service pools. We don't trigger any purge if we
	 * encountered any error while loading the list of services, because
	 * the world exposes a global generation number to manage expirations,
	 * and because without service, we have no way (yet) to find *all* the
	 * slots concerned by any service of a given type. */
	if (*srvtypes) {
		if (!any_loading_error)
			oio_lb_world__increment_generation(lbw);
		oio_lb_world__reload_pools(lbw, lb, SRV.nsinfo);
		_reload_lb_service_types(lbw, lb, srvtypes, tabsrv, taberr);
		oio_lb_world__reload_storage_policies(lbw, lb, SRV.nsinfo);
		if (!any_loading_error)
			oio_lb_world__purge_old_generations(lbw);
	}

	if (taberr) g_ptr_array_free(taberr, TRUE);
	if (tabsrv) g_ptr_array_free(tabsrv, TRUE);
	g_strfreev(srvtypes);
	return NULL;
}

void
sqlx_task_reload_lb (struct sqlx_service_s *ss)
{
	EXTRA_ASSERT(ss != NULL);
	ADAPTIVE_PERIOD_DECLARE();

	if (!grid_main_is_running ())
		return;  /* stopped */
	if (!SRV.nsinfo || !SRV.nsinfo)
		return;  /* not ready */
	if (ADAPTIVE_PERIOD_SKIP())
		return;

	GError *err = _reload_lb_world(ss->lb_world, ss->lb);
	if (err) {
		GRID_WARN("Failed to reload LB world: %s", err->message);
		g_clear_error(&err);
	} else {
		ADAPTIVE_PERIOD_ONSUCCESS(oio_sqlx_lb_refresh_period);
	}
}

/* Specific requests handlers ----------------------------------------------- */

static gboolean
_dispatch_RELOAD (struct gridd_reply_ctx_s *reply, gpointer pss, gpointer i)
{
	(void) i;
	struct sqlx_service_s *ss = pss;
	g_assert (ss != NULL);

	oio_lb_world__flush(ss->lb_world);

	GError *err = _reload_lb_world(ss->lb_world, ss->lb);
	if (err)
		reply->send_error (0, err);
	else
		reply->send_reply (200, "OK");
	return TRUE;
}

static gboolean
_dispatch_FLUSH (struct gridd_reply_ctx_s *reply, gpointer pss, gpointer i)
{
	(void) i;
	struct sqlx_service_s *ss = pss;
	g_assert(ss != NULL);
	if (ss->resolver) {
		hc_resolver_flush_csm0 (ss->resolver);
		hc_resolver_flush_services (ss->resolver);
	}
	reply->send_reply(200, "OK");
	return TRUE;
}

static const struct gridd_request_descr_s *
_get_service_requests (void)
{
	static struct gridd_request_descr_s descriptions[] = {
		{NAME_MSGNAME_SQLX_RELOAD, _dispatch_RELOAD, NULL},
		{NAME_MSGNAME_SQLX_FLUSH,  _dispatch_FLUSH,  NULL},
		{NULL, NULL, NULL}
	};

	return descriptions;
}

