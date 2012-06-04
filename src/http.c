/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *	This program is licensed under the BSD license, which can be found
 *	in the file LICENSE.
 */

/*
 *	http.c: the HTTP server thread, serving status pages and taking position uploads
 */

#include <signal.h>
#include <poll.h>

#include <event2/event.h>  
#include <event2/http.h>  

#include "http.h"
#include "config.h"
#include "hlog.h"
#include "hmalloc.h"
#include "worker.h"

int http_shutting_down;
int http_reconfiguring;

/*
 *	HTTP request router
 */

void http_router(struct evhttp_request *r, void *arg)
{
	const char *uri = evhttp_request_get_uri(r);
}

struct event *ev_timer = NULL;
struct evhttp *libsrvr = NULL;
struct event_base *libbase = NULL;

void http_timer(evutil_socket_t fd, short events, void *arg)
{
	struct timeval http_timer_tv;
	http_timer_tv.tv_sec = 0;
	http_timer_tv.tv_usec = 200000;
	
	//hlog(LOG_DEBUG, "http_timer fired");
	
	if (http_shutting_down) {
		event_base_loopexit(libbase, &http_timer_tv);
		return;
	}
	
	event_add(ev_timer, &http_timer_tv);
}

/*
 *	HTTP server thread
 */

void http_thread(void *asdf)
{
	sigset_t sigs_to_block;
	int e;
	
	struct timeval http_timer_tv;
	
	http_timer_tv.tv_sec = 0;
	http_timer_tv.tv_usec = 200000;
	
	pthreads_profiling_reset("http");
	
	sigemptyset(&sigs_to_block);
	sigaddset(&sigs_to_block, SIGALRM);
	sigaddset(&sigs_to_block, SIGINT);
	sigaddset(&sigs_to_block, SIGTERM);
	sigaddset(&sigs_to_block, SIGQUIT);
	sigaddset(&sigs_to_block, SIGHUP);
	sigaddset(&sigs_to_block, SIGURG);
	sigaddset(&sigs_to_block, SIGPIPE);
	sigaddset(&sigs_to_block, SIGUSR1);
	sigaddset(&sigs_to_block, SIGUSR2);
	pthread_sigmask(SIG_BLOCK, &sigs_to_block, NULL);
	
	/* start the http thread, which will start server threads */
	hlog(LOG_INFO, "HTTP thread starting...");
	
	http_reconfiguring = 1;
	while (!http_shutting_down) {
		if (http_reconfiguring) {
			http_reconfiguring = 0;
			
			// do init
			libbase = event_base_new();
			libsrvr = evhttp_new(libbase);
			ev_timer = event_new(libbase, -1, EV_TIMEOUT, http_timer, NULL);
			event_add(ev_timer, &http_timer_tv);
			
			hlog(LOG_INFO, "HTTP thread ready.");
		}
		
		evhttp_bind_socket(libsrvr, http_bind, http_port);
		evhttp_set_gencb(libsrvr, http_router, NULL);
		event_base_dispatch(libbase);
	}
	
	hlog(LOG_DEBUG, "HTTP thread shutting down...");
}

