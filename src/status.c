/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *	This program is licensed under the BSD license, which can be found
 *	in the file LICENSE.
 *	
 */

/*
 *	Generate the status JSON string for the web status view
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/utsname.h>

#include "status.h"
#include "cellmalloc.h"
#include "hmalloc.h"
#include "config.h"
#include "version.h"
#include "hlog.h"
#include "worker.h"
#include "historydb.h"
#include "dupecheck.h"
#include "filter.h"
#include "incoming.h"
#include "accept.h"
#include "cJSON.h"
#include "counterdata.h"
#include "client_heard.h"

time_t startup_tick;

pthread_mutex_t status_json_mt = PTHREAD_MUTEX_INITIALIZER;
char *status_json_cached = NULL;
time_t status_json_cache_t = 0;

struct cdata_list_t {
	const char *tree;
	const char *name;
	struct cdata_list_t *next;
	struct cdata_t *cd;
	int gauge;
} *cdata_list = NULL;

cJSON *liveupgrade_status = NULL;

/*
 *	status_uname: get operating system name and architecture
 */

#define UNAME_LEN 512
static void status_uname(cJSON *root)
{
	struct utsname ut;
	char s[UNAME_LEN];
	
	if (uname(&ut) < 0) {
		hlog(LOG_ERR, "status_uname: uname() failed: %s", strerror(errno));
		return;
	}
	
	/* no version info, security by obscurity */
	snprintf(s, UNAME_LEN, "%s %s", ut.sysname, ut.machine);
	
	cJSON_AddStringToObject(root, "os", s);
}

/*
 *	Generate a JSON status string
 */

char *status_json_string(int no_cache, int periodical)
{
	char *out = NULL;
	int pe;
	
	/* if we have a very recent status JSON available, return it instead. */
	if (!no_cache) {
		if ((pe = pthread_mutex_lock(&status_json_mt))) {
			hlog(LOG_ERR, "status_json_string(): could not lock status_json_mt: %s", strerror(pe));
			return NULL;
		}
		if (status_json_cached && (status_json_cache_t == tick || status_json_cache_t == tick - 1)) {
			out = hstrdup(status_json_cached);
			if ((pe = pthread_mutex_unlock(&status_json_mt))) {
				hlog(LOG_ERR, "status_json_string(): could not unlock status_json_mt: %s", strerror(pe));
				return NULL;
			}
			return out;
		}
		if ((pe = pthread_mutex_unlock(&status_json_mt))) {
			hlog(LOG_ERR, "status_json_string(): could not unlock status_json_mt: %s", strerror(pe));
			return NULL;
		}
	}
	
	/* Ok, go and build the JSON tree */
	cJSON *root = cJSON_CreateObject();
	
	cJSON *server = cJSON_CreateObject();
	cJSON_AddStringToObject(server, "server_id", serverid);
	cJSON_AddStringToObject(server, "admin", myadmin);
	cJSON_AddStringToObject(server, "email", myemail);
	cJSON_AddStringToObject(server, "software", PROGNAME);
	cJSON_AddStringToObject(server, "software_version", version_build);
	cJSON_AddStringToObject(server, "software_build_time", verstr_build_time);
	cJSON_AddStringToObject(server, "software_build_user", verstr_build_user);
	cJSON_AddNumberToObject(server, "uptime", tick - startup_tick);
	cJSON_AddNumberToObject(server, "t_started", startup_tick);
	cJSON_AddNumberToObject(server, "t_now", tick);
	status_uname(server);
	cJSON_AddItemToObject(root, "server", server);
	
	cJSON *memory = cJSON_CreateObject();
#ifndef _FOR_VALGRIND_
	struct cellstatus_t cellst;
	historydb_cell_stats(&cellst), 
	cJSON_AddNumberToObject(memory, "historydb_cells_used", historydb_cellgauge);
	cJSON_AddNumberToObject(memory, "historydb_cells_free", cellst.freecount);
	cJSON_AddNumberToObject(memory, "historydb_used_bytes", historydb_cellgauge*cellst.cellsize_aligned);
	cJSON_AddNumberToObject(memory, "historydb_allocated_bytes", (long)cellst.blocks * (long)cellst.block_size);
	cJSON_AddNumberToObject(memory, "historydb_block_size", (long)cellst.block_size);
	cJSON_AddNumberToObject(memory, "historydb_blocks", (long)cellst.blocks);
	cJSON_AddNumberToObject(memory, "historydb_blocks_max", (long)cellst.blocks_max);
	cJSON_AddNumberToObject(memory, "historydb_cell_size", cellst.cellsize);
	cJSON_AddNumberToObject(memory, "historydb_cell_size_aligned", cellst.cellsize_aligned);
	cJSON_AddNumberToObject(memory, "historydb_cell_align", cellst.alignment);
	
	dupecheck_cell_stats(&cellst), 
	cJSON_AddNumberToObject(memory, "dupecheck_cells_used", dupecheck_cellgauge);
	cJSON_AddNumberToObject(memory, "dupecheck_cells_free", cellst.freecount);
	cJSON_AddNumberToObject(memory, "dupecheck_used_bytes", dupecheck_cellgauge*cellst.cellsize_aligned);
	cJSON_AddNumberToObject(memory, "dupecheck_allocated_bytes", (long)cellst.blocks * (long)cellst.block_size);
	cJSON_AddNumberToObject(memory, "dupecheck_block_size", (long)cellst.block_size);
	cJSON_AddNumberToObject(memory, "dupecheck_blocks", (long)cellst.blocks);
	cJSON_AddNumberToObject(memory, "dupecheck_blocks_max", (long)cellst.blocks_max);
	cJSON_AddNumberToObject(memory, "dupecheck_cell_size", cellst.cellsize);
	cJSON_AddNumberToObject(memory, "dupecheck_cell_size_aligned", cellst.cellsize_aligned);
	cJSON_AddNumberToObject(memory, "dupecheck_cell_align", cellst.alignment);
	
	struct cellstatus_t cellst_filter, cellst_filter_wx, cellst_filter_entrycall;
	filter_cell_stats(&cellst_filter, &cellst_filter_entrycall, &cellst_filter_wx),
	cJSON_AddNumberToObject(memory, "filter_cells_used", filter_cellgauge);
	cJSON_AddNumberToObject(memory, "filter_cells_free", cellst_filter.freecount);
	cJSON_AddNumberToObject(memory, "filter_used_bytes", filter_cellgauge*cellst_filter.cellsize_aligned);
	cJSON_AddNumberToObject(memory, "filter_allocated_bytes", (long)cellst_filter.blocks * (long)cellst_filter.block_size);
	cJSON_AddNumberToObject(memory, "filter_block_size", (long)cellst_filter.block_size);
	cJSON_AddNumberToObject(memory, "filter_blocks", (long)cellst_filter.blocks);
	cJSON_AddNumberToObject(memory, "filter_blocks_max", (long)cellst_filter.blocks_max);
	cJSON_AddNumberToObject(memory, "filter_cell_size", cellst_filter.cellsize);
	cJSON_AddNumberToObject(memory, "filter_cell_size_aligned", cellst_filter.cellsize_aligned);
	cJSON_AddNumberToObject(memory, "filter_cell_align", cellst_filter.alignment);
	
	cJSON_AddNumberToObject(memory, "filter_wx_cells_used", filter_wx_cellgauge);
	cJSON_AddNumberToObject(memory, "filter_wx_cells_free", cellst_filter_wx.freecount);
	cJSON_AddNumberToObject(memory, "filter_wx_used_bytes", filter_wx_cellgauge*cellst_filter_wx.cellsize_aligned);
	cJSON_AddNumberToObject(memory, "filter_wx_allocated_bytes", (long)cellst_filter_wx.blocks * (long)cellst_filter_wx.block_size);
	cJSON_AddNumberToObject(memory, "filter_wx_block_size", (long)cellst_filter_wx.block_size);
	cJSON_AddNumberToObject(memory, "filter_wx_blocks", (long)cellst_filter_wx.blocks);
	cJSON_AddNumberToObject(memory, "filter_wx_blocks_max", (long)cellst_filter_wx.blocks_max);
	cJSON_AddNumberToObject(memory, "filter_wx_cell_size", cellst_filter_wx.cellsize);
	cJSON_AddNumberToObject(memory, "filter_wx_cell_size_aligned", cellst_filter_wx.cellsize_aligned);
	cJSON_AddNumberToObject(memory, "filter_wx_cell_align", cellst_filter_wx.alignment);
	
	cJSON_AddNumberToObject(memory, "filter_entrycall_cells_used", filter_entrycall_cellgauge);
	cJSON_AddNumberToObject(memory, "filter_entrycall_cells_free", cellst_filter_entrycall.freecount);
	cJSON_AddNumberToObject(memory, "filter_entrycall_used_bytes", filter_entrycall_cellgauge*cellst_filter_entrycall.cellsize_aligned);
	cJSON_AddNumberToObject(memory, "filter_entrycall_allocated_bytes", (long)cellst_filter_entrycall.blocks * (long)cellst_filter_entrycall.block_size);
	cJSON_AddNumberToObject(memory, "filter_entrycall_block_size", (long)cellst_filter_entrycall.block_size);
	cJSON_AddNumberToObject(memory, "filter_entrycall_blocks", (long)cellst_filter_entrycall.blocks);
	cJSON_AddNumberToObject(memory, "filter_entrycall_blocks_max", (long)cellst_filter_entrycall.blocks_max);
	cJSON_AddNumberToObject(memory, "filter_entrycall_cell_size", cellst_filter_entrycall.cellsize);
	cJSON_AddNumberToObject(memory, "filter_entrycall_cell_size_aligned", cellst_filter_entrycall.cellsize_aligned);
	cJSON_AddNumberToObject(memory, "filter_entrycall_cell_align", cellst_filter_entrycall.alignment);
	
	struct cellstatus_t cellst_pbuf_small, cellst_pbuf_medium, cellst_pbuf_large;
	incoming_cell_stats(&cellst_pbuf_small, &cellst_pbuf_medium, &cellst_pbuf_large);
	cJSON_AddNumberToObject(memory, "pbuf_small_cells_used", cellst_pbuf_small.cellcount - cellst_pbuf_small.freecount);
	cJSON_AddNumberToObject(memory, "pbuf_small_cells_free", cellst_pbuf_small.freecount);
	cJSON_AddNumberToObject(memory, "pbuf_small_cells_alloc", cellst_pbuf_small.cellcount);
	cJSON_AddNumberToObject(memory, "pbuf_small_used_bytes", (cellst_pbuf_small.cellcount - cellst_pbuf_small.freecount)*cellst_pbuf_small.cellsize_aligned);
	cJSON_AddNumberToObject(memory, "pbuf_small_allocated_bytes", (long)cellst_pbuf_small.blocks * (long)cellst_pbuf_small.block_size);
	cJSON_AddNumberToObject(memory, "pbuf_small_block_size", (long)cellst_pbuf_small.block_size);
	cJSON_AddNumberToObject(memory, "pbuf_small_blocks", (long)cellst_pbuf_small.blocks);
	cJSON_AddNumberToObject(memory, "pbuf_small_blocks_max", (long)cellst_pbuf_small.blocks_max);
	cJSON_AddNumberToObject(memory, "pbuf_small_cell_size", cellst_pbuf_small.cellsize);
	cJSON_AddNumberToObject(memory, "pbuf_small_cell_size_aligned", cellst_pbuf_small.cellsize_aligned);
	cJSON_AddNumberToObject(memory, "pbuf_small_cell_align", cellst_pbuf_small.alignment);
	
	cJSON_AddNumberToObject(memory, "pbuf_medium_cells_used", cellst_pbuf_medium.cellcount - cellst_pbuf_medium.freecount);
	cJSON_AddNumberToObject(memory, "pbuf_medium_cells_free", cellst_pbuf_medium.freecount);
	cJSON_AddNumberToObject(memory, "pbuf_medium_cells_alloc", cellst_pbuf_medium.cellcount);
	cJSON_AddNumberToObject(memory, "pbuf_medium_used_bytes", (cellst_pbuf_medium.cellcount - cellst_pbuf_medium.freecount)*cellst_pbuf_medium.cellsize_aligned);
	cJSON_AddNumberToObject(memory, "pbuf_medium_allocated_bytes", (long)cellst_pbuf_medium.blocks * (long)cellst_pbuf_medium.block_size);
	cJSON_AddNumberToObject(memory, "pbuf_medium_block_size", (long)cellst_pbuf_medium.block_size);
	cJSON_AddNumberToObject(memory, "pbuf_medium_blocks", (long)cellst_pbuf_medium.blocks);
	cJSON_AddNumberToObject(memory, "pbuf_medium_blocks_max", (long)cellst_pbuf_medium.blocks_max);
	cJSON_AddNumberToObject(memory, "pbuf_medium_cell_size", cellst_pbuf_medium.cellsize);
	cJSON_AddNumberToObject(memory, "pbuf_medium_cell_size_aligned", cellst_pbuf_medium.cellsize_aligned);
	cJSON_AddNumberToObject(memory, "pbuf_medium_cell_align", cellst_pbuf_medium.alignment);
	
	cJSON_AddNumberToObject(memory, "pbuf_large_cells_used", cellst_pbuf_large.cellcount - cellst_pbuf_large.freecount);
	cJSON_AddNumberToObject(memory, "pbuf_large_cells_free", cellst_pbuf_large.freecount);
	cJSON_AddNumberToObject(memory, "pbuf_large_cells_alloc", cellst_pbuf_large.cellcount);
	cJSON_AddNumberToObject(memory, "pbuf_large_used_bytes", (cellst_pbuf_large.cellcount - cellst_pbuf_large.freecount)*cellst_pbuf_large.cellsize_aligned);
	cJSON_AddNumberToObject(memory, "pbuf_large_allocated_bytes", (long)cellst_pbuf_large.blocks * (long)cellst_pbuf_large.block_size);
	cJSON_AddNumberToObject(memory, "pbuf_large_block_size", (long)cellst_pbuf_large.block_size);
	cJSON_AddNumberToObject(memory, "pbuf_large_blocks", (long)cellst_pbuf_large.blocks);
	cJSON_AddNumberToObject(memory, "pbuf_large_blocks_max", (long)cellst_pbuf_large.blocks_max);
	cJSON_AddNumberToObject(memory, "pbuf_large_cell_size", cellst_pbuf_large.cellsize);
	cJSON_AddNumberToObject(memory, "pbuf_large_cell_size_aligned", cellst_pbuf_large.cellsize_aligned);
	cJSON_AddNumberToObject(memory, "pbuf_large_cell_align", cellst_pbuf_large.alignment);
	
	struct cellstatus_t cellst_client_heard;
	client_heard_cell_stats(&cellst_client_heard);
	cJSON_AddNumberToObject(memory, "client_heard_cells_used", cellst_client_heard.cellcount - cellst_client_heard.freecount);
	cJSON_AddNumberToObject(memory, "client_heard_cells_free", cellst_client_heard.freecount);
	cJSON_AddNumberToObject(memory, "client_heard_cells_alloc", cellst_client_heard.cellcount);
	cJSON_AddNumberToObject(memory, "client_heard_used_bytes", (cellst_client_heard.cellcount - cellst_client_heard.freecount)*cellst_client_heard.cellsize_aligned);
	cJSON_AddNumberToObject(memory, "client_heard_allocated_bytes", (long)cellst_client_heard.blocks * (long)cellst_client_heard.block_size);
	cJSON_AddNumberToObject(memory, "client_heard_block_size", (long)cellst_client_heard.block_size);
	cJSON_AddNumberToObject(memory, "client_heard_blocks", (long)cellst_client_heard.blocks);
	cJSON_AddNumberToObject(memory, "client_heard_blocks_max", (long)cellst_client_heard.blocks_max);
	cJSON_AddNumberToObject(memory, "client_heard_cell_size", cellst_client_heard.cellsize);
	cJSON_AddNumberToObject(memory, "client_heard_cell_size_aligned", cellst_client_heard.cellsize_aligned);
	cJSON_AddNumberToObject(memory, "client_heard_cell_align", cellst_client_heard.alignment);
#endif
	
	cJSON_AddItemToObject(root, "memory", memory);
	
	cJSON *historydb = cJSON_CreateObject();
	cJSON_AddNumberToObject(historydb, "inserts", historydb_inserts);
	cJSON_AddNumberToObject(historydb, "lookups", historydb_lookups);
	cJSON_AddNumberToObject(historydb, "hashmatches", historydb_hashmatches);
	cJSON_AddNumberToObject(historydb, "keymatches", historydb_keymatches);
	cJSON_AddNumberToObject(historydb, "noposcount", historydb_noposcount);
	cJSON_AddNumberToObject(historydb, "cleaned", historydb_cleanup_cleaned);
	cJSON_AddItemToObject(root, "historydb", historydb);
	
	cJSON *dupecheck = cJSON_CreateObject();
	cJSON_AddNumberToObject(dupecheck, "dupes_dropped", dupecheck_dupecount);
	cJSON_AddNumberToObject(dupecheck, "uniques_out", dupecheck_outcount);
	cJSON_AddItemToObject(root, "dupecheck", dupecheck);
	
	cJSON *json_totals = cJSON_CreateObject();
	cJSON *json_listeners = cJSON_CreateArray();
	accept_listener_status(json_listeners, json_totals);
	cJSON_AddItemToObject(root, "totals", json_totals);
	cJSON_AddItemToObject(root, "listeners", json_listeners);
	
	cJSON *json_clients = cJSON_CreateArray();
	cJSON *json_uplinks = cJSON_CreateArray();
	cJSON *json_peers = cJSON_CreateArray();
	cJSON *json_workers = cJSON_CreateArray();
	worker_client_list(json_workers, json_clients, json_uplinks, json_peers, json_totals, memory);
	cJSON_AddItemToObject(root, "workers", json_workers);
	cJSON_AddItemToObject(root, "uplinks", json_uplinks);
	cJSON_AddItemToObject(root, "peers", json_peers);
	cJSON_AddItemToObject(root, "clients", json_clients);
	
	/* if this is a periodical per-minute dump, collect historical data */
	if (periodical) {
		cJSON *ct, *cv;
		struct cdata_list_t *cl;
		for (cl = cdata_list; (cl); cl = cl->next) {
			ct = cJSON_GetObjectItem(root, cl->tree);
			cv = cJSON_GetObjectItem(ct, cl->name);
			
			/* cJSON's cv->valueint is just an integer, which will overflow
			 * too quickly. So, let's take the more expensive valuedouble.
			 */
			if (cl->gauge)
				cdata_gauge_sample(cl->cd, (cv) ? cv->valuedouble : -1);
			else
				cdata_counter_sample(cl->cd, (cv) ? cv->valuedouble : -1);
		}
	}
	
	cJSON_AddNumberToObject(json_totals, "tcp_bytes_rx_rate", cdata_get_last_value("totals.tcp_bytes_rx") / CDATA_INTERVAL);
	cJSON_AddNumberToObject(json_totals, "tcp_bytes_tx_rate", cdata_get_last_value("totals.tcp_bytes_tx") / CDATA_INTERVAL);
	cJSON_AddNumberToObject(json_totals, "udp_bytes_rx_rate", cdata_get_last_value("totals.udp_bytes_rx") / CDATA_INTERVAL);
	cJSON_AddNumberToObject(json_totals, "udp_bytes_tx_rate", cdata_get_last_value("totals.udp_bytes_tx") / CDATA_INTERVAL);
	cJSON_AddNumberToObject(json_totals, "bytes_rx_rate", (cdata_get_last_value("totals.tcp_bytes_rx") + cdata_get_last_value("totals.udp_bytes_rx")) / CDATA_INTERVAL);
	cJSON_AddNumberToObject(json_totals, "bytes_tx_rate", (cdata_get_last_value("totals.tcp_bytes_tx") + cdata_get_last_value("totals.udp_bytes_tx")) / CDATA_INTERVAL);
	
	cJSON *json_rx_errs = cJSON_CreateStringArray(inerr_labels, INERR_BUCKETS);
	cJSON_AddItemToObject(root, "rx_errs", json_rx_errs);
	
	/* the tree is built, print it out to a malloc'ed string */
	out = cJSON_Print(root);
	cJSON_Delete(root);
	
	/* cache it */
	if ((pe = pthread_mutex_lock(&status_json_mt))) {
		hlog(LOG_ERR, "status_json_string(): could not lock status_json_mt: %s", strerror(pe));
                                return NULL;
	}
	if (status_json_cached)
		hfree(status_json_cached);
		
	status_json_cached = hstrdup(out);
	status_json_cache_t = tick;
	
	if ((pe = pthread_mutex_unlock(&status_json_mt))) {
		hlog(LOG_ERR, "status_json_string(): could not unlock status_json_mt: %s", strerror(pe));
		return NULL;
	}
	
	return out;
}

#define PATHLEN 500

int json_write_file(char *basename, const char *s)
{
	char path[PATHLEN+1];
	char tmppath[PATHLEN+1];
	FILE *fp;
	time_t start_t, end_t;
	
	time(&start_t);
	
	snprintf(path, PATHLEN, "%s/%s.json", rundir, basename);
	snprintf(tmppath, PATHLEN, "%s.tmp", path);
	fp = fopen(tmppath,"w");
	if (!fp) {
		hlog(LOG_ERR, "json file write failed: Could not open %s for writing: %s", tmppath, strerror(errno));
		return -1;
	}
	
	if (fputs(s, fp) == EOF) {
		hlog(LOG_ERR, "json file write failed: Could not write to %s: %s", tmppath, strerror(errno));
		fclose(fp);
		return -1;
	}
	
	if (fclose(fp)) {
		hlog(LOG_ERR, "json file update failed: close(%s): %s", tmppath, strerror(errno));
		return -1;
	}
	
	if (rename(tmppath, path)) {
		hlog(LOG_ERR, "json file update failed: Could not rename %s to %s: %s", tmppath, path, strerror(errno));
		return -1;
	}
	
	/* check if we're having I/O delays */
	time(&end_t);
	if (end_t - start_t > 2) {
		hlog(LOG_ERR, "json file update took %d seconds", end_t - start_t);
	}
	
	return 0;
}

/*
 *	Status dumping to file is currently disabled, since doing any
 *	significant I/O seems to have the risk of blocking the whole process
 *	when the server is running under VMWare.
 */

#ifdef ENABLE_STATUS_DUMP_FILE

static int status_dump_fp(FILE *fp)
{
	char *out = status_json_string(1, 1);
	fputs(out, fp);
	hfree(out);
	
	return 0;
}

int status_dump_file(void)
{
	char path[PATHLEN+1];
	char tmppath[PATHLEN+1];
	FILE *fp;
	time_t start_t, end_t;
	
	time(&start_t);
	
	snprintf(path, PATHLEN, "%s/aprsc-status.json", rundir);
	snprintf(tmppath, PATHLEN, "%s.tmp", path);
	fp = fopen(tmppath,"w");
	if (!fp) {
		hlog(LOG_ERR, "status file update failed: Could not open %s for writing: %s", tmppath, strerror(errno));
		return -1;
	}
	
	status_dump_fp(fp);
	
	if (fclose(fp)) {
		hlog(LOG_ERR, "status file update failed: close(%s): %s", tmppath, strerror(errno));
		return -1;
	}
	
	if (rename(tmppath, path)) {
		hlog(LOG_ERR, "status file update failed: Could not rename %s to %s: %s", tmppath, path, strerror(errno));
		return -1;
	}
	
	/* check if we're having I/O delays */
	time(&end_t);
	if (end_t - start_t > 2) {
		hlog(LOG_ERR, "status file update took %d seconds", end_t - start_t);
	}
	
	return 0;
}

#else

/*
 *	The code to update counterdata is embedded in the status JSON
 *	generator. If dump-status-to-file-per-minute is disabled,
 *	do generate the JSON just to update the counters for graphs.
 *	TODO: just update the cdata counters without generating the JSON.
 */

int status_dump_file(void)
{
	time_t start_t, end_t;
	
	time(&start_t);
	
	char *out = status_json_string(1, 1);
	hfree(out);
	
	/* check if we're having delays */
	time(&end_t);
	if (end_t - start_t > 2) {
		hlog(LOG_ERR, "status counters update took %d seconds", end_t - start_t);
	}
	
	return 0;
}

#endif

/*
 *	Save enough status to a JSON file so that live upgrade can continue
 *	serving existing clients with it
 */

int status_dump_liveupgrade(void)
{
	const char *out;
	
	if (!worker_shutdown_clients) {
		return 0;
	}
	
	cJSON *root = cJSON_CreateObject();
	cJSON_AddItemToObject(root, "clients", worker_shutdown_clients);
	
	out = cJSON_Print(root);
	cJSON_Delete(root);
	worker_shutdown_clients = NULL;
	
	return json_write_file("liveupgrade", out);
}

int status_read_liveupgrade(void)
{
	char path[PATHLEN+1];
	FILE *fp;
	char *s = NULL;
	int sl = 0;
	char buf[32768];
	int i;
	
	snprintf(path, PATHLEN, "%s/liveupgrade.json", rundir);
	
	hlog(LOG_DEBUG, "Live upgrade: reading status from %s", path);
	
	fp = fopen(path, "r");
	if (!fp) {
		hlog(LOG_ERR, "liveupgrade dump file read failed: Could not open %s for reading: %s", path, strerror(errno));
		return -1;
	}
	
	while ((i = fread(buf, 1, sizeof(buf), fp)) > 0) {
		//hlog(LOG_DEBUG, "read %d bytes", i);
		s = hrealloc(s, sl + i+1);
		memcpy(s + sl, buf, i);
		sl += i;
		s[sl] = 0; // keep null-terminated
		//hlog(LOG_DEBUG, "now: %s", s);
	}
	
	if (fclose(fp)) {
		hlog(LOG_ERR, "liveupgrade dump file read failed: close(%s): %s", path, strerror(errno));
		return -1;
	}
	
	/* decode JSON */
	cJSON *dec = cJSON_Parse(s);
	if (!dec) {
		hlog(LOG_ERR, "liveupgrade dump parsing failed");
		return -1;
	}
	
	liveupgrade_status = dec;
	
	return 0;
}

void status_init(void)
{
	int i;
	char *n;
	
	static const char *cdata_start[][3] = {
		{ "totals", "clients", "g" },
		{ "totals", "connects", "c" },
		{ "totals", "tcp_bytes_rx", "c" },
		{ "totals", "tcp_bytes_tx", "c" },
		{ "totals", "udp_bytes_rx", "c" },
		{ "totals", "udp_bytes_tx", "c" }, 
		{ "totals", "tcp_pkts_rx", "c" },
		{ "totals", "tcp_pkts_tx", "c" },
		{ "totals", "udp_pkts_rx", "c" },
		{ "totals", "udp_pkts_tx", "c" }, 
		{ "dupecheck", "dupes_dropped", "c" },
		{ "dupecheck", "uniques_out", "c" },
		{ NULL, NULL }
	};
	
	i = 0;
	while (cdata_start[i][0] != NULL) {
		n = hmalloc(strlen(cdata_start[i][0]) + 1 + strlen(cdata_start[i][1]) + 1);
		sprintf(n, "%s.%s", cdata_start[i][0], cdata_start[i][1]);
		struct cdata_list_t *cl = hmalloc(sizeof(*cl));
		cl->tree = cdata_start[i][0];
		cl->name = cdata_start[i][1];
		cl->next = cdata_list;
		cl->cd = cdata_alloc(n);
		hfree(n);
		cl->gauge = cdata_start[i][2][0] == 'g' ? 1 : 0;
		cdata_list = cl;
		i++;
	}
}

void status_atend(void)
{
	struct cdata_list_t *cl, *cl_next;
	int pe;
	
	for (cl = cdata_list; (cl); cl = cl_next) {
		cl_next = cl->next;
		cdata_free(cl->cd);
		hfree(cl);
	}
	
	if ((pe = pthread_mutex_lock(&status_json_mt))) {
		hlog(LOG_ERR, "status_atend(): could not lock status_json_mt: %s", strerror(pe));
		return;
	}
	
	if (status_json_cached) {
		hfree(status_json_cached);
		status_json_cached = NULL;
	}
	
	if ((pe = pthread_mutex_unlock(&status_json_mt))) {
		hlog(LOG_ERR, "status_atend(): could not unlock status_json_mt: %s", strerror(pe));
		return;
	}
}

