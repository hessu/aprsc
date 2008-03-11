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

#define HELPS	"Usage: aprsc [-c cfgfile] [-f (fork)] [-n <logname>] [-e <loglevel>] [-o <logdest>] [-r <logdir>] [-h (help)]\n"

#include <pthread.h>
#include <semaphore.h>

#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>

#include "hmalloc.h"
#include "hlog.h"
#include "config.h"
#include "accept.h"
#include "uplink.h"
#include "worker.h"

#include "dupecheck.h"
#include "filter.h"
#include "historydb.h"
#include "crc32.h"

struct itimerval itv; // Linux profiling timer does not pass over to pthreads..

int shutting_down = 0;		// are we shutting down now?
int reopen_logs = 0;		// should we reopen log files now?
int reconfiguring = 0;		// should we reconfigure now?
int uplink_simulator;

/*
 *	Parse arguments
 */

void parse_cmdline(int argc, char *argv[])
{
	int s, i;
	int failed = 0;
	
	while ((s = getopt(argc, argv, "c:fn:r:d:e:o:?h")) != -1) {
	switch (s) {
		case 'c':
			cfgfile = hstrdup(optarg);
			break;
		case 'f':
			fork_a_daemon = 1;
			break;
		case 'n':
			logname = hstrdup(optarg);
			break;
		case 'r':
			log_dir = hstrdup(optarg);
			break;
		case 'd':
			if (!strcasecmp(optarg, "requests")) {
				//dump_requests = 1;
			} else if (!strcasecmp(optarg, "splay")) {
				dump_splay = 1;
			} else {
				fprintf(stderr, "Unknown -d parameter: %s\n", optarg);
				failed = 1;
			}
			break;
		case 'e':
			i = pick_loglevel(optarg, log_levelnames);
			if (i > -1)
				log_level = i;
			else {
				fprintf(stderr, "Log level unknown: \"%s\"\n", optarg);
				failed = 1;
			}
			break;
		case 'o':
			i = pick_loglevel(optarg, log_destnames);
			if (i > -1)
				log_dest = i;
			else {
				fprintf(stderr, "Log destination unknown: \"%s\"\n", optarg);
				failed = 1;
			}
			break;
		case '?':
		case 'h':
			fprintf(stderr, "%s", VERSTR);
			failed = 1;
	}
	}
	
	if ((log_dest == L_FILE) && (!log_dir)) {
		fprintf(stderr, "Log destination set to 'file' but no log directory specified!\n");
		failed = 1;
	}
	
	if (failed) {
		fputs(HELPS, stderr);
		exit(failed);
	}
}

/*
 *	signal handler
 */
 
int sighandler(int signum)
{
	switch (signum) {

	case SIGINT:
	case SIGTERM:
	case SIGQUIT:
		hlog(LOG_CRIT, "Shutting down on signal %d", signum);
		shutting_down = 1;
		return 0;
		
	case SIGHUP:
		hlog(LOG_INFO, "SIGHUP received: reopening logs");
		reopen_logs = 1;
		break;
		
	case SIGUSR1:
		hlog(LOG_INFO, "SIGUSR1 received: reconfiguring");
		reconfiguring = 1;
		break;
		
	default:
		hlog(LOG_WARNING, "* SIG %d ignored", signum);
		break;
	}
	
	signal(signum, (void *)sighandler);	/* restore handler */
	return 0;
		
}

/*
 *	A very Linux specific thing, as there the pthreads are a special variation
 *	of fork(), and per POSIX the profiling timers are not kept over fork()...
 */
void pthreads_profiling_reset(void)
{
	if (itv.it_interval.tv_usec || itv.it_interval.tv_sec) {
	  setitimer(ITIMER_PROF, &itv, NULL);
	}
}


/*
 *	Main
 */

int main(int argc, char **argv)
{
	pthread_t accept_th;
	int e;
	
	/* close stdin */
	close(0);
	time(&now);
	setlinebuf(stdout);
	setlinebuf(stderr);

	getitimer(ITIMER_PROF, &itv);
	
	/* command line */
	parse_cmdline(argc, argv);
	
	/* open syslog, write an initial log message and read configuration */
	open_log(logname, 0);
	hlog(LOG_NOTICE, "Starting up...");
	if (read_config()) {
		hlog(LOG_CRIT, "Initial configuration failed.");
		exit(1);
	}
	
	/* fork a daemon */
	if (fork_a_daemon) {
		int i = fork();
		if (i < 0) {
			hlog(LOG_CRIT, "Fork to background failed: %s", strerror(errno));
			fprintf(stderr, "Fork to background failed: %s\n", strerror(errno));
			exit(1);
		} else if (i == 0) {
			/* child */
		} else {
			/* parent, quitting */
			hlog(LOG_DEBUG, "Forked daemon process %d, parent quitting", i);
			exit(0);
		}
	}
	
	/* write pid file, now that we have our final pid... might fail, which is critical */
	if (!writepid(pidfile))
		exit(0);
	
	/* catch signals */
	signal(SIGINT, (void *)sighandler);
	signal(SIGTERM, (void *)sighandler);
	signal(SIGQUIT, (void *)sighandler);
	signal(SIGHUP, (void *)sighandler);
	signal(SIGUSR1, (void *)sighandler);
	signal(SIGUSR2, (void *)sighandler);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGURG, SIG_IGN);
	


	/* Early inits in single-thread mode */
	crcinit();
	filter_init();
	pbuf_init();
	dupecheck_init();
	historydb_init();


	/* start the accept thread, which will start server threads */
	if (pthread_create(&accept_th, NULL, (void *)accept_thread, NULL))
		perror("pthread_create failed for accept_thread");

	/* act as statistics and housekeeping thread from now on */
	while (!shutting_down) {
		poll(NULL, 0, 300); // 0.300 sec -- or there abouts..
		if (!uplink_simulator)
			time(&now);
		
		if (reopen_logs) {
			reopen_logs = 0;
			close_log(1);
		}
		if (reconfiguring) {
			reconfiguring = 0;
			if (read_config()) {
				hlog(LOG_ERR, "New configuration was not successfully read.");
			} else {
				hlog(LOG_INFO, "New configuration read successfully. Applying....");
				accept_reconfiguring = 1;
			}
		}

		/* 
		if (now >= next_expiry) {
			next_expiry = now + expiry_interval;
			// expire
			// pbuf_expire();
		}
		if (now >= next_stats) {
			next_stats = now + stats_interval;
			// log stats
		}
		*/

	}
	
	hlog(LOG_INFO, "Signalling accept_thread to shut down...");
	accept_shutting_down = 1;
	if ((e = pthread_join(accept_th, NULL)))
		hlog(LOG_ERR, "Could not pthread_join accept_th: %s", strerror(e));
	else
		hlog(LOG_INFO, "Accept thread has terminated.");
	
	// sp_free_freelist();

	free_config();
	dupecheck_atend();
	historydb_atend();
	
	hlog(LOG_CRIT, "Shut down.");
	close_log(0);
	
	return 0;
}

