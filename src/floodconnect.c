/*
 *	floodconnect
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *	This program is licensed under the BSD license, which can be found
 *	in the file LICENSE.
 *	
 */

#define HELPS	"Usage: floodconnect [-t <threads>] [-n <parallel-conns-per-thread>] [-a <close-after-seconds>] [-i (end-after-seconds)]\n"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <locale.h>

int threads = 1;
int parallel_conns_per_thread = 1;
int close_after_seconds = 2;
int end_after_seconds = 5;

struct floodthread_t {
	struct floodthread_t *next;
	
	pthread_t th;
};

pthread_attr_t pthr_attrs;

/*
 *	Parse arguments
 */

void parse_cmdline(int argc, char *argv[])
{
	int s;
	int failed = 0;
	
	while ((s = getopt(argc, argv, "t:n:a:i:?h")) != -1) {
	switch (s) {
		case 't':
			threads = atoi(optarg);
			break;
		case 'n':
			parallel_conns_per_thread = atoi(optarg);
			break;
		case 'a':
			close_after_seconds = atoi(optarg);
			break;
		case 'i':
			end_after_seconds = atoi(optarg);
			break;
		case '?':
		case 'h':
			failed = 1;
	}
	}
	
	if (failed) {
		fputs(HELPS, stderr);
		exit(failed);
	}
}

/*
 *	testing thread
 */

void flood_thread(struct floodthread_t *self)
{
	fprintf(stderr, "thread starting\n");
}

void run_test(void)
{
	int t;
	int e;
	struct floodthread_t *thrs = NULL;
	struct floodthread_t *th;
	
	/* start threads up */
	for (t = 0; t < threads; t++) {
		th = malloc(sizeof(*th));
		if (!th) {
			fprintf(stderr, "out of memory!");
			exit(1);
		}
		memset(th, 0, sizeof(*th));
		
		th->next = thrs;
		thrs = th;
		
		/* start the worker thread */
		if (pthread_create(&th->th, &pthr_attrs, (void *)flood_thread, th))
		perror("pthread_create failed for flood_thread");
	}
	
	/* wait for threads to quit */
	for (th = thrs; (th); th = th->next) {
		if ((e = pthread_join(th->th, NULL))) {
			perror("Could not pthread_join flood_thread");
		} else {
			fprintf(stderr, "thread has ended\n");
		}
	}
}

/*
 *	Main
 */

int main(int argc, char **argv)
{
	/* set locale to C for consistent output in testing scripts */
	if (!setlocale(LC_CTYPE, "C")) {
		perror("Failed to set locale C for LC_CTYPE.");
		exit(1);
	}
	if (!setlocale(LC_MESSAGES, "C")) {
		perror("Failed to set locale C for LC_MESSAGES.");
		exit(1);
	}
	if (!setlocale(LC_NUMERIC, "C")) {
		perror("Failed to set locale C for LC_NUMERIC.");
		exit(1);
	}
	
	/* 128 kB stack is enough for each thread,
	   default of 10 MB is way too much...*/
	pthread_attr_init(&pthr_attrs);
	pthread_attr_setstacksize(&pthr_attrs, 128*1024);

	
	/* command line */
	parse_cmdline(argc, argv);
	
	run_test();
	
	return 0;
}

