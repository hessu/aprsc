/*
 *	eventload
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *	This program is licensed under the BSD license, which can be found
 *	in the file LICENSE.
 *	
 */

#define HELPS	"Usage: eventload [-t <threads>] [-n <parallel-conns-per-thread>] [-a <sleep-msec>] [-i (end-after-seconds)]\n"

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
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <ctype.h>

int threads = 1;
int parallel_conns_per_thread = 3;
int rows_between_sleep = 30;
int sleep_msec = 1000;
int sleep_conn_msec = 10;
int end_after_seconds = 5;

time_t start_t;
time_t end_t;

struct floodthread_t {
        int id;
	struct floodthread_t *next;
	

	pthread_t th;
};

struct addrinfo *ai;

pthread_attr_t pthr_attrs;

/* As of April 11 2000 Steve Dimse has released this code to the open
 * source aprs community
 *
 * (from aprsd sources)
 */

#define kKey 0x73e2		// This is the key for the data

short aprs_passcode(const char* theCall)
{
	char rootCall[10];	// need to copy call to remove ssid from parse
	char *p1 = rootCall;
	
	while ((*theCall != '-') && (*theCall != 0) && (p1 < rootCall + 9))
		*p1++ = toupper(*theCall++);
	
	*p1 = 0;
	
	short hash = kKey;		// Initialize with the key value
	short i = 0;
	short len = strlen(rootCall);
	char *ptr = rootCall;
	
	while (i < len) {		// Loop through the string two bytes at a time
		hash ^= (*ptr++)<<8;	// xor high byte with accumulated hash
		hash ^= (*ptr++);	// xor low byte with accumulated hash
		i += 2;
	}
	
	return hash & 0x7fff;		// mask off the high bit so number is always positive
}

/*
 *	Parse arguments
 */

void parse_cmdline(int argc, char *argv[])
{
	int s;
	int failed = 0;
	struct addrinfo req;
	
	while ((s = getopt(argc, argv, "t:n:a:i:u:p:?h")) != -1) {
	switch (s) {
		case 't':
			threads = atoi(optarg);
			break;
		case 'n':
			parallel_conns_per_thread = atoi(optarg);
			break;
		case 'a':
			sleep_msec = atoi(optarg);
			break;
		case 'i':
			end_after_seconds = atoi(optarg);
			break;
		case '?':
		case 'h':
			failed = 1;
	}
	}
	
	if (optind + 2 != argc) {
		fprintf(stderr, "invalid number of parameters\n");
		exit(1);
	}

	memset(&req, 0, sizeof(req));
	req.ai_family   = 0;
	req.ai_socktype = SOCK_STREAM;
	req.ai_protocol = IPPROTO_TCP;
	req.ai_flags    = 0;
	ai = NULL;
	
	s = getaddrinfo(argv[optind], argv[optind+1], &req, &ai);
	if (s != 0) {
		fprintf(stderr, "Aaddress parsing or hostname lookup failure for %s: %s\n", argv[optind], gai_strerror(s));
		exit(1);
	}
	
	if (failed) {
		fputs(HELPS, stderr);
		exit(failed);
	}
}

/*
 *	testing thread
 */

#define WBUFLEN 16384

void flood_round(struct floodthread_t *self)
{
	int *fds;
	int i;
	char username[32];
	char login_cmd[WBUFLEN+1];
	int login_len;
	char wbuf[WBUFLEN+1];
	int wbufpos;
	char rbuf[WBUFLEN+1];
	int rbufpos;
	long long round;
	struct epoll_event *evs;  // event flags for each fd.
	int epollfd;
#define MAX_EPOLL_EVENTS 500
	struct epoll_event events[MAX_EPOLL_EVENTS];
	
	fprintf(stderr, "%d: flood_round starting\n", self->id);
	
	login_len = strlen(login_cmd);
	
	epollfd = epoll_create(1024);
	if (epollfd < 0) {
		fprintf(stderr, "xflood_thread: epoll_create failed: %s\n", strerror(errno));
		return;
	}
	
	fds = malloc(sizeof(int) * parallel_conns_per_thread);
	if (!fds) {
		fprintf(stderr, "flood_thread: out of memory, could not allocate fds\n");
		return;
	}
	
	evs = malloc(sizeof(*evs) * parallel_conns_per_thread);
	if (!evs) {
		fprintf(stderr, "flood_thread: out of memory, could not allocate evs\n");
		return;
	}
	
	int arg = 1;
	
	for (i = 0; i < parallel_conns_per_thread; i++) {
	        if (i % 100 == 0)
	                fprintf(stderr, "%d: now %d connected\n", self->id, i);
		fds[i] = -1;
		
		int fd;
		if ((fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) < 0) {
			perror("socket");
			exit(1);
		}
		fds[i] = fd;
		
		if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&arg, sizeof(arg))) {
			perror("Failed to set SO_REUSEADDR on new socket");
			exit(1);
		}
		
		if (connect(fd, ai->ai_addr, ai->ai_addrlen)) {
			fprintf(stderr, "connect failed: %s\n", strerror(errno));
			close(fd);
			fds[i] = -1;
			continue;
		}
		
		sprintf(username, "%03x%05x", getpid(), fd);
		wbufpos = snprintf(wbuf, WBUFLEN, "user %s pass %d\r\n", username, aprs_passcode(username));
		int w = write(fd, wbuf, wbufpos);
		if (w != wbufpos) {
		        fprintf(stderr, "%d: failed to write login command for client %d: %s\n", self->id, i, strerror(errno));
		}
		
		evs[i].events   = EPOLLIN|EPOLLOUT; // | EPOLLET ?
		// Each event has initialized callback pointer to struct xpoll_fd_t...
		evs[i].data.ptr = &fds[i];
		if (epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &evs[i]) == -1) {
		        fprintf(stderr, "epoll_ctl EPOL_CTL_ADD %d failed: %s\n", fd, strerror(errno));
		        exit(1);
                }
	        usleep(sleep_conn_msec*1000);
	}
	
	fprintf(stderr, "%d: all connected\n", self->id);
	
	int n, nfds;
	int ebufpos;
	while (1) {
	        round++;
	        wbufpos = snprintf(wbuf, WBUFLEN, "FOOBAR>TEST,qAR,TEST:test %lld", round);
	        
	        nfds = epoll_wait(epollfd, events, MAX_EPOLL_EVENTS, 1000);
	        
	        for (n = 0; n < nfds; ++n) {
	        	// Each event has initialized callback pointer to struct xpoll_fd_t...
	        	int *xfd = (int*) events[n].data.ptr;
	        	
	        	if (events[n].events & (EPOLLIN|EPOLLPRI)) {
				rbufpos = read(*xfd, rbuf, WBUFLEN);
				if (rbufpos > 0) {
					//fprintf(stderr, "%d read %d: %.*s\n", *xfd, rbufpos, rbufpos, rbuf);
				} else {
					fprintf(stderr, "%d got %d\n", *xfd, rbufpos);
				}
			}
			
		}
		
		nfds = round % parallel_conns_per_thread;
		for (i = 0; i < 10; i++) {
			n = nfds + (parallel_conns_per_thread / 12)*i;
			n = n % parallel_conns_per_thread;
			n = fds[n];
			//fprintf(stderr, "writing on %d\n", n);
			ebufpos = wbufpos + snprintf(wbuf + wbufpos, WBUFLEN-wbufpos, "-%d\r\n", n);
			write(n, wbuf, ebufpos);
		}
		
	        usleep(sleep_msec*1000);
        }
	
	fprintf(stderr, "%d: end of round\n", self->id);
	
	for (i = 0; i < parallel_conns_per_thread; i++) {
		if (fds[i] == -1)
			continue;
		close(fds[i]);
		fds[i] = -1;
	}
	
	close(epollfd);
	free(fds);
	free(evs);
}


void flood_thread(struct floodthread_t *self)
{
	fprintf(stderr, "thread %d starting\n", self->id);
	
	while (1) {
		flood_round(self);
	}
}

void run_test(void)
{
	int t;
	int e;
	struct floodthread_t *thrs = NULL;
	struct floodthread_t *th;
	
	time(&start_t);
	
	/* start threads up */
	for (t = 0; t < threads; t++) {
		th = malloc(sizeof(*th));
		if (!th) {
			fprintf(stderr, "out of memory!");
			exit(1);
		}
		memset(th, 0, sizeof(*th));
		th->id = t;
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
	
	time(&end_t);
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
	
	signal(SIGPIPE, SIG_IGN);
	
	run_test();
	
	return 0;
}

