/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *	This program is licensed under the BSD license, which can be found
 *	in the file LICENSE.
 *	
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

#include "status.h"
#include "config.h"
#include "hlog.h"
#include "worker.h"
#include "historydb.h"
#include "dupecheck.h"
#include "filter.h"
#include "cJSON.h"

time_t startup_tick;

int status_dump_fp(FILE *fp)
{
	char *out;
	
	cJSON *root = cJSON_CreateObject();
	
	cJSON *server = cJSON_CreateObject();
	cJSON_AddStringToObject(server, "software", "aprsc");
	cJSON_AddStringToObject(server, "software_version", VERSION);
	cJSON_AddNumberToObject(server, "uptime", tick - startup_tick);
	cJSON_AddNumberToObject(server, "t_started", startup_tick);
	cJSON_AddNumberToObject(server, "t_now", tick);
	cJSON_AddItemToObject(root, "server", server);
	
	// TODO: add free counts of each cell pool
	cJSON *memory = cJSON_CreateObject();
	cJSON_AddNumberToObject(memory, "cells_historydb", historydb_cellgauge);
	cJSON_AddNumberToObject(memory, "cells_dupecheck", dupecheck_cellgauge);
	cJSON_AddNumberToObject(memory, "cells_filter_entrycall", filter_entrycall_cellgauge);
	cJSON_AddNumberToObject(memory, "cells_filter_wx", filter_wx_cellgauge);
	cJSON_AddNumberToObject(memory, "bytes_historydb", historydb_cellgauge*HISTORYDB_CELL_SIZE);
	cJSON_AddNumberToObject(memory, "bytes_dupecheck", dupecheck_cellgauge*DUPECHECK_CELL_SIZE);
	cJSON_AddItemToObject(root, "memory", memory);
	
	cJSON *dupecheck = cJSON_CreateObject();
	cJSON_AddNumberToObject(dupecheck, "dupes_dropped", dupecheck_dupecount);
	cJSON_AddNumberToObject(dupecheck, "uniques_out", dupecheck_outcount);
	cJSON_AddItemToObject(root, "dupecheck", dupecheck);
	
	out = cJSON_Print(root);
	cJSON_Delete(root);
	fputs(out, fp);
	free(out);
	
	return 0;
}

#define PATHLEN 500

int status_dump_file(void)
{
	char path[PATHLEN+1];
	char tmppath[PATHLEN+1];
	FILE *fp;
	
	snprintf(path, PATHLEN, "%s/aprsc-status.json", rundir);
	snprintf(tmppath, PATHLEN, "%s.tmp", path);
	fp = fopen(tmppath,"w");
	if (!fp) {
		hlog(LOG_ERR, "status dump failed: Could not open %s for writing: %s", tmppath, strerror(errno));
		return -1;
	}
	
	status_dump_fp(fp);
	
	if (fclose(fp)) {
		hlog(LOG_ERR, "status dump failed: close(%s): %s", tmppath, strerror(errno));
		return -1;
	}
	
	if (rename(tmppath, path)) {
		hlog(LOG_ERR, "status dump failed: Could not rename %s to %s: %s", tmppath, path, strerror(errno));
		return -1;
	}
	
	return 0;
}