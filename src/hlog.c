/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *	
 */

/*
 *	log.c
 *
 *	logging facility with configurable log levels and
 *	logging destinations
 */

#include <stdio.h>
#include <stdarg.h>
#include <syslog.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include "hlog.h"
#include "hmalloc.h"
#include "rwlock.h"

int log_dest = L_DEFDEST;	/* Logging destination */
int log_level = LOG_INFO;	/* Logging level */
int log_facility = LOG_LOCAL1;	/* Logging facility */
char *log_name = NULL;		/* Logging name */

char log_basename[] = "hemserv.log";
char *log_dir = NULL;		/* Access log directory */
char *log_fname = NULL;		/* Access log file name */
int log_file = -1;		/* If logging to a file, the file name */
rwlock_t log_file_lock = RWL_INITIALIZER;

char accesslog_basename[] = "hemserv.access.log";
char *accesslog_dir = NULL;	/* Access log directory */
char *accesslog_fname = NULL;	/* Access log file name */
int accesslog_file = -1;	/* Access log fd */
rwlock_t accesslog_lock = RWL_INITIALIZER;

char *log_levelnames[] = {
	"EMERG",
	"ALERT",
	"CRIT",
	"ERR",
	"WARNING",
	"NOTICE",
	"INFO",
	"DEBUG",
	NULL
};

char *log_destnames[] = {
	"none",
	"stderr",
	"syslog",
	"file",
	NULL
};


/*
 *	Append a formatted string to a dynamically allocated string
 */

char *str_append(char *s, const char *fmt, ...)
{
	va_list args;
	char buf[LOG_LEN];
	int len;
	char *ret;
	
	va_start(args, fmt);
	vsnprintf(buf, LOG_LEN, fmt, args);
	va_end(args);
	buf[LOG_LEN-1] = 0;
	
	len = strlen(s);
	ret = hrealloc(s, len + strlen(buf) + 1);
	strcpy(ret + len, buf);
	
	return ret;
}

/*
 *	Pick a log level
 */

int pick_loglevel(char *s, char **names)
{
	int i;
	
	for (i = 0; (names[i]); i++)
		if (!strcasecmp(s, names[i]))
			return i;
			
	return -1;
}

/*
 *	Open log
 */
 
int open_log(char *name, int reopen)
{
	if (!reopen)
		rwl_wrlock(&log_file_lock);
		
	if (log_name)
		hfree(log_name);
		
	if (!(log_name = hstrdup(name))) {
		fprintf(stderr, "hemserv logger: out of memory!\n");
		exit(1);
	}
	
	if (log_dest == L_SYSLOG)
		openlog(name, LOG_NDELAY|LOG_PID, log_facility);
	
	if (log_dest == L_FILE) {
		if (log_fname)
			hfree(log_fname);
		
		log_fname = hmalloc(strlen(log_dir) + strlen(log_basename) + 2);
		sprintf(log_fname, "%s/%s", log_dir, log_basename);
		
		log_file = open(log_fname, O_WRONLY|O_CREAT|O_APPEND, S_IRUSR|S_IWUSR|S_IRGRP);
		if (log_file < 0) {
			fprintf(stderr, "hemserv logger: Could not open %s: %s\n", log_fname, strerror(errno));
			exit(1);
		}
	}
	
	rwl_wrunlock(&log_file_lock);
	
	if (log_dest == L_FILE)
		hlog(LOG_DEBUG, "Log file %s %sopened on fd %d", log_fname, (reopen) ? "re" : "", log_file);
	
	return 0;
}

/*
 *	Close log
 */
 
int close_log(int reopen)
{
	char *s = hstrdup(log_name);
	
	rwl_wrlock(&log_file_lock);
	
	if (log_name) {
		hfree(log_name);
		log_name = NULL;
	}
	
	if (log_dest == L_SYSLOG) {
		closelog();
	} else if (log_dest == L_FILE) {
		if (close(log_file))
			fprintf(stderr, "hemserv logger: Could not close log file %s: %s\n", log_fname, strerror(errno));
		log_file = -1;
		hfree(log_fname);
		log_fname = NULL;
	}
	
	if (reopen)
		open_log(s, 1);
	
	if (!reopen)
		rwl_wrunlock(&log_file_lock);
	
	hfree(s);
	
	return 0;
}

/*
 *	Log a message
 */

int hlog(int priority, const char *fmt, ...)
{
	va_list args;
	char s[LOG_LEN];
	char wb[LOG_LEN];
	int len, w;
	struct tm lt;
	struct timeval tv;
	
	if (priority > 7)
		priority = 7;
	else if (priority < 0)
		priority = 0;
	
	if (priority > log_level)
		return 0;
	
	va_start(args, fmt);
	vsnprintf(s, LOG_LEN, fmt, args);
	va_end(args);

#if 1
	gettimeofday(&tv, NULL);
#else
	time(&tv.tv_sec);  //  tv.tv_sec = now   SHOULD BE ENOUGH
	tv.tv_usec = 0;
#endif
	gmtime_r(&tv.tv_sec, &lt);
	
	if (log_dest == L_STDERR) {
		rwl_rdlock(&log_file_lock);
		fprintf(stderr, "%4d/%02d/%02d %02d:%02d:%02d.%06d %s[%d:%lu] %s: %s\n",
			lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec, (int)tv.tv_usec,
			(log_name) ? log_name : "hemserv", (int)getpid(), (unsigned long int)pthread_self(), log_levelnames[priority], s);
		rwl_rdunlock(&log_file_lock);
		
	} else if ((log_dest == L_FILE) && (log_file >= 0)) {
		len = snprintf(wb, LOG_LEN, "%4d/%02d/%02d %02d:%02d:%02d.%06d %s[%d:%ld] %s: %s\n",
			       lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec, (int)tv.tv_usec,
			       (log_name) ? log_name : "hemserv", (int)getpid(), (unsigned long int)pthread_self(), log_levelnames[priority], s);
		wb[LOG_LEN-1] = 0;
		rwl_rdlock(&log_file_lock);
		if ((w = write(log_file, wb, len)) != len)
			fprintf(stderr, "hemserv logger: Could not write to %s (fd %d): %s\n", log_fname, log_file, strerror(errno));
		rwl_rdunlock(&log_file_lock);
		
	} else if (log_dest == L_SYSLOG) {
		rwl_rdlock(&log_file_lock);
		syslog(priority, "%s: %s", log_levelnames[priority], s);
		rwl_rdunlock(&log_file_lock);
	}
	
	return 1;
}


/*
 *	Open access log
 */

int accesslog_open(char *logd, int reopen)
{
	if (!reopen)
		rwl_wrlock(&accesslog_lock);
	
	if (accesslog_fname)
		hfree(accesslog_fname);
		
	if (accesslog_dir)
		hfree(accesslog_dir);
		
	accesslog_dir = hstrdup(logd);
	accesslog_fname = hmalloc(strlen(accesslog_dir) + strlen(accesslog_basename) + 2);
	sprintf(accesslog_fname, "%s/%s", accesslog_dir, accesslog_basename);
	
	accesslog_file = open(accesslog_fname, O_WRONLY|O_CREAT|O_APPEND, S_IRUSR|S_IWUSR|S_IRGRP);
	if (accesslog_file < 0)
		hlog(LOG_CRIT, "Could not open %s: %s", accesslog_fname, strerror(errno));
	
	rwl_wrunlock(&accesslog_lock);
	
	return accesslog_file;
}

/*
 *	Close access log
 */
 
int accesslog_close(char *reopenpath)
{
	hlog(LOG_DEBUG, "Closing access log...");
	rwl_wrlock(&accesslog_lock);
	hlog(LOG_DEBUG, "Closing access log, got lock");
	
	if (close(accesslog_file))
		hlog(LOG_CRIT, "Could not close %s: %s", accesslog_fname, strerror(errno));
	hfree(accesslog_fname);
	hfree(accesslog_dir);
	accesslog_fname = accesslog_dir = NULL;
	accesslog_file = -1;
	
	if (reopenpath) {
		return accesslog_open(reopenpath, 1);
	} else {
		rwl_wrunlock(&accesslog_lock);
		return 0;
	}
}

/*
 *	Log an access log message
 */

int accesslog(const char *fmt, ...)
{
	va_list args;
	char s[LOG_LEN], wb[LOG_LEN];
	time_t t;
	struct tm lt;
	int len;
	ssize_t w;
	
	va_start(args, fmt);
	vsnprintf(s, LOG_LEN, fmt, args);
	va_end(args);
	s[LOG_LEN-1] = 0;
	
	time(&t);
	gmtime_r(&t, &lt);
	
	len = snprintf(wb, LOG_LEN, "[%4.4d/%2.2d/%2.2d %2.2d:%2.2d:%2.2d] %s\n",
		lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec, s);
	wb[LOG_LEN-1] = 0;
	
	rwl_rdlock(&accesslog_lock);
	if (accesslog_file >= 0) {
		if ((w = write(accesslog_file, wb, len)) != len)
			hlog(LOG_CRIT, "Could not write to %s (fd %d): %s", accesslog_fname, accesslog_file, strerror(errno));
	} else {
		if (accesslog_file != -666) {
			hlog(LOG_ERR, "Access log not open, log lines are lost!");
			accesslog_file = -666;
		}
	}
	rwl_rdunlock(&accesslog_lock);
	
	return 1;
}

/*
 *	Write my PID to file
 *	FIXME: add flock(TRY) to prevent multiple copies from running
 */

int writepid(char *name)
{
	FILE *f;
	
	if (!(f = fopen(name, "w"))) {
		hlog(LOG_CRIT, "Could not open %s for writing: %s",
			name, strerror(errno));
		return 0;
	}
	if (fprintf(f, "%ld\n", (long)getpid()) < 0) {
		hlog(LOG_CRIT, "Could not write to %s: %s",
			name, strerror(errno));
		return 0;
	}
	
	if (fclose(f)) {
		hlog(LOG_CRIT, "Could not close %s: %s",
			name, strerror(errno));
		return 0;
	}
	
	return 1;
}

