/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *
 */

/*
 *	aprsis2.h: the APRS-IS2 Protocol Buffers based protocol
 */

#ifndef APRSIS2_H
#define APRSIS2_H

#include "worker.h"

extern void is2_allocate_obuf(struct client_t *c);

extern int is2_input_handler_login(struct worker_t *self, struct client_t *c, Aprsis2__IS2Message *m);
extern int is2_input_handler_uplink_wait_signature(struct worker_t *self, struct client_t *c, Aprsis2__IS2Message *m);
extern int is2_input_handler(struct worker_t *self, struct client_t *c, Aprsis2__IS2Message *m);

extern int is2_out_server_signature(struct worker_t *self, struct client_t *c);
extern int is2_deframe_input(struct worker_t *self, struct client_t *c, int start_at);
extern int is2_corepeer_deframe_input(struct worker_t *self, struct client_t *c, char *ibuf, int len);

extern void is2_pbuf_init_packet(struct pbuf_t *pb, Aprsis2__ISPacket *is2_packet_in, Aprsis2__ISPacket *is2_input_template);
extern int is2_write_packet(struct worker_t *self, struct client_t *c, struct pbuf_t *pb);
extern int is2_obuf_flush(struct worker_t *self, struct client_t *c);
extern int is2_corepeer_obuf_flush(struct worker_t *self, struct client_t *c);
extern void is2_obuf_flush_pending(struct worker_t *self);

extern int is2_out_ping(struct worker_t *self, struct client_t *c);
extern int is2_ping_rtt_stats(struct client_t *c, double *avg, double *max, time_t *period);

/* the IS2 frame delimiters */
#define STX 0x02
#define ETX 0x03

/* the in-band upgrade of plaintext client connections */
#define IS2_UPGRADE_HELLO_PREFIX "#is2 hello"
#define IS2_UPGRADE_HELLO_PREFIX_LEN 10

extern int is2_upgrade_offered(struct client_t *c);
extern int is2_upgrade_hello_in(struct worker_t *self, struct client_t *c, const char *s, int len);

/* the UDP peer handshake datagrams start with this */
#define IS2_COREPEER_CONTROL_PREFIX "#is2 peer "
#define IS2_COREPEER_CONTROL_PREFIX_LEN 10

extern int is2_corepeer_propose(struct worker_t *self, struct client_t *c);
extern void is2_corepeer_downgrade(struct worker_t *self, struct client_t *c);
extern int is2_corepeer_control_in(struct worker_t *self, struct client_t *c, char *p, int len);

extern int is2_corepeer_key_setup(void);
extern int is2_corepeer_enabled(void);
extern int is2_wire_selfcheck(void);

#endif

