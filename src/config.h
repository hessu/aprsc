
#ifndef CONFIG_H
#define CONFIG_H

#define PROGNAME "aprsc"
#define VERSION "0.0.1"
#define VERSTR  PROGNAME " v" VERSION
#define SERVERID PROGNAME " " VERSION
#define CRLF "\r\n"

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <inttypes.h>

extern int fork_a_daemon;	/* fork a daemon */

extern int dump_requests;	/* print requests */
extern int dump_splay;		/* print splay tree information */

extern int workers_configured;	/* number of workers to run */

extern int stats_interval;
extern int expiry_interval;

extern int obuf_size;
extern int ibuf_size;

extern int verbose;

extern char *mycall;
extern char *myemail;
extern char *myadmin;

extern char *cfgfile;
extern char *pidfile;
extern char *rundir;
extern char *logdir;
extern char *logname;

struct listen_config_t {
	struct listen_config_t *next;
	struct listen_config_t **prevp; /* pointer to the *next pointer in the previous node */
	
	char *name;			/* name of socket */
	char *host;			/* hostname or dotted-quad IP to bind the UDP socket to, default INADDR_ANY */
	int port;			/* port to bind */

	char *filters[10];		/* up to 10 filters, NULL when not defined */
};

extern struct listen_config_t *listen_config;

extern int read_config(void);
extern void free_config(void);

#endif

