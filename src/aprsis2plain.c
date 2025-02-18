
#include <string.h>

#include "aprsis2plain.h"
#include "hmalloc.h"
#include "hlog.h"

void is2_plain_metadata_reset(struct client_t *c)
{
	// only use the received plain metadata for the immediately next packet
	c->is2_input_template.optional_rx_rssi_case = APRSIS2__ISPACKET__OPTIONAL_RX_RSSI__NOT_SET;
	c->is2_input_template.optional_rx_snr_db_case = APRSIS2__ISPACKET__OPTIONAL_RX_SNR_DB__NOT_SET;
}

static void is2_plain_metadata_key(struct client_t *c,
	const char *key, int key_len,
	const char *val, int val_len)
{
	if (key_len == 7 && memcmp(key, "rx_rssi", 7) == 0) {
		errno = 0;
		c->is2_input_template.rx_rssi = strtof(val, NULL);
		if (errno == 0)
			c->is2_input_template.optional_rx_rssi_case = APRSIS2__ISPACKET__OPTIONAL_RX_RSSI_RX_RSSI;
	} else if (key_len == 9 && memcmp(key, "rx_snr_db", 9) == 0) {
		errno = 0;
		c->is2_input_template.rx_snr_db = strtof(val, NULL);
		if (errno == 0)
			c->is2_input_template.optional_rx_snr_db_case = APRSIS2__ISPACKET__OPTIONAL_RX_SNR_DB_RX_SNR_DB;
	}
}

/*
 *	Decode key-value metadata from plain APRS-IS clients, so that
 *	metadata can be sent without implementing IS2.
 */
int is2_plain_metadata(struct worker_t *self, struct client_t *c, const char *s, int len)
{
	hlog(LOG_DEBUG, "is2-meta: '%.*s'", len, s);

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
			is2_plain_metadata_key(c, &s[key_start], key_len, &s[value_start], value_len);	
	}

	return 0;
}
