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
 *	aprsis2plain.c: IS2 metadata from plaintext clients, given as
 *	key-value pairs on #is2-meta comment lines, and the key-value
 *	parser shared with the IS2 handshake lines
 */

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <math.h>

#include "aprsis2plain.h"
#include "hmalloc.h"
#include "hlog.h"

void is2_plain_metadata_reset(struct client_t *c)
{
	// only use the received plain metadata for the immediately next packet
	c->is2_input_template.optional_rx_rssi_case = APRSIS2__ISPACKET__OPTIONAL_RX_RSSI__NOT_SET;
	c->is2_input_template.optional_rx_snr_db_case = APRSIS2__ISPACKET__OPTIONAL_RX_SNR_DB__NOT_SET;
	c->is2_input_template.rx_modem = APRSIS2__ISPACKET__MODEM_TYPE__MODEM_UNDEFINED;
	c->is2_input_template.optional_rx_bitrate_case = APRSIS2__ISPACKET__OPTIONAL_RX_BITRATE__NOT_SET;
	c->is2_input_template.optional_rx_frequency_case = APRSIS2__ISPACKET__OPTIONAL_RX_FREQUENCY__NOT_SET;
}

/*
 *	Parse a metadata value: a finite decimal number, making up the whole
 *	value. Returns 1 with the number in *out, or 0 if the value is not
 *	one, in which case it is ignored.
 */
static int is2_plain_metadata_number(const char *val, int val_len, double *out)
{
	char buf[32];
	char *end;
	double d;

	if (val_len <= 0 || val_len >= sizeof(buf))
		return 0;
	memcpy(buf, val, val_len);
	buf[val_len] = 0;

	errno = 0;
	d = strtod(buf, &end);
	if (errno != 0 || end != buf + val_len || !isfinite(d))
		return 0;

	*out = d;
	return 1;
}

/*
 *	Parse a metadata value for a float field: a number which is finite
 *	as a float too, not only as a double.
 */
static int is2_plain_metadata_float(const char *val, int val_len, float *out)
{
	double d;

	if (!is2_plain_metadata_number(val, val_len, &d) || !isfinite((float)d))
		return 0;

	*out = (float)d;
	return 1;
}

/*
 *	Parse a key-value value which is a whole number from min to max.
 *	Returns 1 with the number in *out, or 0 if the value is not one.
 */
int is2_kv_int(const char *val, int val_len, long min, long max, long *out)
{
	char buf[32];
	char *end;
	long l;

	if (val_len <= 0 || val_len >= sizeof(buf))
		return 0;
	memcpy(buf, val, val_len);
	buf[val_len] = 0;

	errno = 0;
	l = strtol(buf, &end, 10);
	if (errno != 0 || end != buf + val_len || l < min || l > max)
		return 0;

	*out = l;
	return 1;
}

static void is2_plain_metadata_key(void *ctx,
	const char *key, int key_len,
	const char *val, int val_len)
{
	struct client_t *c = ctx;
	double d;
	float f;
	long l;

	if (key_len == 7 && memcmp(key, "rx_rssi", 7) == 0) {
		if (is2_plain_metadata_float(val, val_len, &f)) {
			c->is2_input_template.rx_rssi = f;
			c->is2_input_template.optional_rx_rssi_case = APRSIS2__ISPACKET__OPTIONAL_RX_RSSI_RX_RSSI;
		}
	} else if (key_len == 9 && memcmp(key, "rx_snr_db", 9) == 0) {
		if (is2_plain_metadata_float(val, val_len, &f)) {
			c->is2_input_template.rx_snr_db = f;
			c->is2_input_template.optional_rx_snr_db_case = APRSIS2__ISPACKET__OPTIONAL_RX_SNR_DB_RX_SNR_DB;
		}
	} else if (key_len == 8 && memcmp(key, "rx_modem", 8) == 0) {
		/* one of the modem types defined in aprsis2.proto, looked up in
		 * the generated enum descriptor so that new types need no change
		 * here; 0 is MODEM_UNDEFINED, which is what an absent key means
		 */
		if (is2_kv_int(val, val_len, 1, INT32_MAX, &l)
		    && protobuf_c_enum_descriptor_get_value(&aprsis2__ispacket__modem_type__descriptor, l))
			c->is2_input_template.rx_modem = l;
	} else if (key_len == 10 && memcmp(key, "rx_bitrate", 10) == 0) {
		if (is2_kv_int(val, val_len, 1, UINT32_MAX, &l)) {
			c->is2_input_template.rx_bitrate = l;
			c->is2_input_template.optional_rx_bitrate_case = APRSIS2__ISPACKET__OPTIONAL_RX_BITRATE_RX_BITRATE;
		}
	} else if (key_len == 12 && memcmp(key, "rx_frequency", 12) == 0) {
		if (is2_plain_metadata_number(val, val_len, &d) && d > 0) {
			c->is2_input_template.rx_frequency = d;
			c->is2_input_template.optional_rx_frequency_case = APRSIS2__ISPACKET__OPTIONAL_RX_FREQUENCY_RX_FREQUENCY;
		}
	}
}

/*
 *	Decode key-value metadata from plain APRS-IS clients, so that
 *	metadata can be sent without implementing IS2.
 */
int is2_plain_metadata(struct worker_t *self, struct client_t *c, const char *s, int len)
{
	hlog(LOG_DEBUG, "is2-meta: '%.*s'", len, s);

	is2_kv_parse(s, len, is2_plain_metadata_key, c);

	return 0;
}

/*
 *	Split a line of space-separated key=value pairs, as used on the
 *	#is2-meta lines and in the IS2 handshake lines, and call key_cb for
 *	each pair with a non-empty key and value. A bare word without '='
 *	is skipped alone, so that the pairs after it still apply. The values
 *	are not NUL-terminated.
 */
void is2_kv_parse(const char *s, int len, is2_kv_cb_t key_cb, void *ctx)
{
	for (int i = 0; i < len; i++) {
		// skip whitespace
		for (; i < len; i++) {
			if (s[i] != ' ')
				break;
		}
		int key_start = i;
		int key_len = 0;
		for (; i < len; i++) {
			if (s[i] == '=') {
				key_len = i-key_start;
				break;
			}
			if (s[i] == ' ')
				break;
		}
		if (i >= len || s[i] != '=') {
			// a bare word without '=' is not a pair: skip it alone,
			// so that the pairs after it still apply
			continue;
		}
		//if (key_len > 0)
		//	hlog(LOG_DEBUG, "is2-meta key: '%.*s'", key_len, &s[key_start]);
		i++;
		int value_start = i;
		for (; i < len; i++) {
			if (s[i] == ' ')
				break;
		}
		int value_len = i - value_start;
		//if (value_len > 0)
		//	hlog(LOG_DEBUG, "is2-meta value: '%.*s'", value_len, &s[value_start]);
		if (key_len > 0 && value_len > 0)
			key_cb(ctx, &s[key_start], key_len, &s[value_start], value_len);
	}
}

/*
 *	Pick a protocol version from a comma-separated list of versions
 *	offered by the other end ("v=2,3"): the highest one from min to max.
 *	Returns 0 if there is none, or the list is not valid.
 */
int is2_kv_version_pick(const char *val, int val_len, int min, int max)
{
	int best = 0;
	int i = 0;

	while (i < val_len) {
		int v = 0;
		int digits = 0;

		for (; i < val_len && val[i] >= '0' && val[i] <= '9'; i++) {
			if (v < 100000)
				v = v * 10 + (val[i] - '0');
			digits++;
		}

		if (digits == 0 || (i < val_len && val[i] != ','))
			return 0;
		i++; /* skip the comma */

		if (v >= min && v <= max && v > best)
			best = v;
	}

	return best;
}
