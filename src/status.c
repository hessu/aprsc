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
#include "cellmalloc.h"
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
