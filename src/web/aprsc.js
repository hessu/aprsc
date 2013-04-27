<!--

var options = {};

function top_status(c, s)
{
	if (!c) {
		$('#status').hide('fast');
		return;
	}
	
	$('#status').hide().html('<div class="' + c + '">' + s + '</div>').show('fast');
}

function isUndefined(v)
{
	var undef;
	return v === undef;
}

function cancel_events(e)
{
	if (!e)
		if (window.event) e = window.event;
	else
		return;
	
	if (e.cancelBubble != null) e.cancelBubble = true;
	if (e.stopPropagation) e.stopPropagation();
	if (e.preventDefault) e.preventDefault();
	if (window.event) e.returnValue = false;
	if (e.cancel != null) e.cancel = true;
}

function parse_options(s)
{
	options = {};
	var a = s.split(' ');
	for (var i = 0; i < a.length; i++) {
		var p = a[i].split('=');
		var c = p[0].toLowerCase();
		options[c] = p[1];
	}
	
	if (options['showemail'] == 1)
		key_translate['email'] = 'Admin email';
}

function server_status_host(s)
{
	var h = s['addr_rem'];
	var p = h.lastIndexOf(':');
	if (h.lastIndexOf(']') > p || p == -1)
		return h + ':14501';
	return h.substr(0, p) + ':14501';
}

function addr_loc_port(s)
{
	return s.substr(s.lastIndexOf(':') + 1);
}

function username_link(s)
{
	return '<a href="http://aprs.fi/?call=' + s + '" target="_blank">' + htmlent(s) + "</a>";
}

function conv_verified(c, k)
{
	if (c['verified'] == 3)
		return '<span class="link" onclick="return cert_popup(event, ' + c['fd'] + ');">Cert</span>';
		
	if (c['verified'])
		return 'Yes';
	
	return '<span class="red">No</span>';
}

function port_conn_rates(c, k)
{
	var ckey = 'l_' + c['proto'] + c['addr'] + ':' + k;
	var c = calc_rate(ckey + ':c', c['connects'], 1);
	if (!isUndefined(c[1]) && c[1] !== '')
		return c[1];
	return '';
}

function client_bytes_rates(c, k)
{
	var ckey;
	if (isUndefined(c['addr_rem']))
		ckey = 'l_' + c['proto'] + c['addr'] + ':' + k;
	else
		ckey = c['addr_rem'] + ':' + k;
	var tx = calc_rate(ckey + ':tx', c['bytes_tx'], 1);
	var rx = calc_rate(ckey + ':rx', c['bytes_rx'], 1);
	if (!isUndefined(tx[1]) && tx[1] !== '')
		return tx[1] + ' / ' + rx[1];
	return '';
}

function client_pkts_rx(c, k)
{
	//if (isUndefined(c['pkts_ign']))
	//	return c['pkts_rx'];
	
	var s = c['pkts_rx'] + '/' + c['pkts_dup'] + '/' + c['pkts_ign'];
	
	if (c['pkts_ign'] > 0 || c['pkts_dup'] > 0)
		return '<span class="'
			+ ((c['pkts_ign'] / c['pkts_rx'] > 0.1) ? 'rxerr_red' : 'rxerr')
			+ '" onclick="return rx_err_popup(event, ' + c['fd'] + ');">' + s + '</span>';

	return s;
}

function htmlent(s)
{
	if (isUndefined(s))
		return '';
		
	return $('<div/>').text(s).html();
}

function lz(i)
{
	if (i < 10)
		return '0' + i;
	
	return i;
}

function timestr(i)
{
	var D = new Date(i*1000);
	return D.getUTCFullYear() + '-' + lz(D.getUTCMonth()+1) + '-' + lz(D.getUTCDate())
		+ ' ' + lz(D.getUTCHours()) + ':' + lz(D.getUTCMinutes()) + ':' + lz(D.getUTCSeconds())
		+ 'z';
}

function dur_str(i)
{
	var t;
	var s = '';
	var c = 0;
	
	if (i > 86400) {
		t = Math.floor(i/86400);
		i -= t*86400;
		s += t + 'd';
		c++;
	}
	if (i > 3600) {
		t = Math.floor(i / 3600);
		i -= t*3600;
		s += t + 'h';
		c++;
	}
	if (c > 1)
		return s;
		
	if (i > 60) {
		t = Math.floor(i / 60);
		i -= t*60;
		s += t + 'm';
		c++;
	}
	
	if (c)
		return s;
	
	return i.toFixed(0) + 's';
}

function conv_none(s)
{
	return s;
}

var listeners_table, uplinks_table, peers_table, clients_table, memory_table,
	dupecheck_table, dupecheck_more_table, totals_table;

var alarm_strings = {
	'no_uplink': "Server does not have any uplink connections.",
	'packet_drop_hang': "Server has dropped packets due to forward time leaps or hangs caused by resource starvation.",
	'packet_drop_future': "Server has dropped packets due to backward time leaps."
};

var rx_err_strings = {
	"unknown": 'Unknown error',
	"no_colon": 'No colon (":") in packet',
	"no_dst": 'No ">" in packet to mark beginning of destination callsign',
	"no_path": 'No path found between source callsign and ":"',
	"inv_srccall": 'Invalid source callsign',
	"no_body": 'No packet body/data after ":"',
	"inv_dstcall": 'Invalid destination callsign',
	"disallow_unverified": 'Packet from unverified local client',
	"disallow_unverified_path": 'Packet from unverified client (TCPXX)',
	"path_nogate": 'Packet with NOGATE/RFONLY in path',
	"party_3rd_ip": '3rd-party packet gated TCPIP>RF>TCPIP',
	"party_3rd_inv": 'Invalid 3rd-party packet header',
	"general_query": 'General query',
	"aprsc_oom_pbuf": 'aprsc out of packet buffers',
	"aprsc_class_fail": 'aprsc failed to classify packet',
	"aprsc_q_bug": 'aprsc Q construct processing failed',
	"q_drop": 'Q construct algorithm dropped packet',
	"short_packet": 'Packet too short',
	"long_packet": 'Packet too long',
	"inv_path_call": 'Invalid callsign in path',
	"q_qax": 'qAX: Packet from unverified remote client',
	"q_qaz": 'qAZ construct',
	"q_path_mycall": 'My ServerID in Q path',
	"q_path_call_twice": 'Same callsign twice in the Q path',
	"q_path_login_not_last": 'Local client login found but not last in Q path',
	"q_path_call_is_local": 'Callsign in Q path is a local verified client',
	"q_path_call_inv": 'Invalid callsign in Q path',
	"q_qau_path_call_srccall": 'qAU callsign in path equals srccall',
	"q_newq_buffer_small": 'New Q construct too big',
	"q_nonval_multi_q_calls": 'Multiple callsigns in Q path from unverified client',
	"q_i_no_viacall": 'I path has no viacall',
	"inerr_empty": 'Empty packet'
};

var key_translate = {
	// server block
	'server_id': 'Server ID',
	'admin': 'Server admin',
	'software': 'Server software',
	'software_build_features': 'Software features',
	'os': 'Operating system',
	'time_started': 'Server started',
	'uptime': 'Uptime',
	
	// dupecheck block
	'dupes_dropped': 'Duplicate packets dropped',
	'uniques_out': 'Unique packets seen',
	
	// dupecheck_more (variations) block
	'exact': 'Exact duplicates',
	'space_trim': 'Dupes with spaces trimmed from end',
	'8bit_strip': 'Dupes with 8-bit bytes stripped out',
	'8bit_clear': 'Dupes with 8th bit set to 0',
	'8bit_spaced': 'Dupes with 8-bit bytes replaced with spaces',
	'low_strip': 'Dupes with low bytes stripped out',
	'low_spaced': 'Dupes with low bytes replaced with spaces',
	'del_strip': 'Dupes with DEL bytes stripped out',
	'del_spaced': 'Dupes with DEL bytes replaced with spaces',
	
	// totals block
	'clients': 'Clients',
	'connects': 'Connects',
	'tcp_pkts_tx': 'Packets Tx TCP',
	'tcp_pkts_rx': 'Packets Rx TCP',
	'tcp_bytes_tx': 'Bytes Tx TCP',
	'tcp_bytes_rx': 'Bytes Rx TCP',
	'udp_pkts_tx': 'Packets Tx UDP',
	'udp_pkts_rx': 'Packets Rx UDP',
	'udp_bytes_tx': 'Bytes Tx UDP',
	'udp_bytes_rx': 'Bytes Rx UDP',
	'sctp_pkts_tx': 'Packets Tx SCTP',
	'sctp_pkts_rx': 'Packets Rx SCTP',
	'sctp_bytes_tx': 'Bytes Tx SCTP',
	'sctp_bytes_rx': 'Bytes Rx SCTP'
};

var key_tooltips = {
	// dupecheck block
	'dupes_dropped': 'Duplicate APRS-IS packets dropped in the dupecheck thread (per-client counts not available for performance reasons)',
	'uniques_out': 'Unique packets passed by the dupecheck thread',
	
	// totals block
	'clients': 'Number of clients allocated currently (including Uplinks, Peers and pseudoclients for UDP listener sockets)',
	'connects': 'Number of accepted TCP connections since startup',
	'tcp_pkts_tx': 'APRS-IS packets transmitted over a TCP connection',
	'tcp_pkts_rx': 'APRS-IS packets received over a TCP connection',
	'tcp_bytes_tx': 'APRS-IS data transmitted over a TCP connection',
	'tcp_bytes_rx': 'APRS-IS data received over a TCP connection',
	'udp_pkts_tx': 'APRS-IS packets transmitted over UDP',
	'udp_pkts_rx': 'APRS-IS packets received over UDP',
	'udp_bytes_tx': 'APRS-IS data transmitted over UDP',
	'udp_bytes_rx': 'APRS-IS data received over UDP',
	'sctp_pkts_tx': 'APRS-IS packets transmitted over SCTP',
	'sctp_pkts_rx': 'APRS-IS packets received over SCTP',
	'sctp_bytes_tx': 'APRS-IS data transmitted over SCTP',
	'sctp_bytes_rx': 'APRS-IS data received over SCTP'
};

var val_convert_c = {
	'bytes_rates': client_bytes_rates,
	'pkts_rx': client_pkts_rx,
	'connects_rates': port_conn_rates,
	'verified': conv_verified
};

var val_convert = {
	'time_started': timestr,
	'uptime': dur_str,
	't_connect': timestr,
	'since_connect': dur_str,
	'since_last_read': dur_str,
	'addr_rem_shown': conv_none,
	'username': username_link,
	'addr_loc': addr_loc_port
};

var listener_cols = {
	'proto': 'Proto',
	'addr': 'Address',
	'name': 'Name',
	'clients': 'Clients',
	'clients_peak': 'Peak',
	'clients_max': 'Max',
	'connects': 'Connects',
	'connects_rates': 'Conn/s',
	'pkts_tx': 'Packets Tx',
	'pkts_rx': 'Packets Rx',
	'bytes_tx': 'Bytes Tx',
	'bytes_rx': 'Bytes Rx',
	'bytes_rates': 'Tx/Rx bytes/s'
};

var uplink_cols = {
	'username': 'Server ID',
	'addr_rem_shown': 'Address',
	'mode': 'Mode',
	't_connect': 'Connected',
	'since_connect': 'Up',
	'since_last_read': 'Last in',
	'show_app_name': 'Software',
	'pkts_tx': 'Packets Tx',
	'pkts_rx': 'Packets Rx',
	'bytes_tx': 'Bytes Tx',
	'bytes_rx': 'Bytes Rx',
	'bytes_rates': 'Tx/Rx bytes/s',
	'obuf_q': 'OutQ'
};

var peer_cols = {
	'username': 'Server ID',
	'addr_rem_shown': 'Address',
	'since_last_read': 'Last in',
	'pkts_tx': 'Packets Tx',
	'pkts_rx': 'Packets Rx',
	'bytes_tx': 'Bytes Tx',
	'bytes_rx': 'Bytes Rx',
	'bytes_rates': 'Tx/Rx bytes/s',
	'obuf_q': 'OutQ'
};

var client_cols = {
	'addr_loc': 'Port',
	'username': 'Callsign',
	'addr_rem_shown': 'Address',
	'verified': 'Verified',
	'since_connect': 'Up',
	'since_last_read': 'Last in',
	'show_app_name': 'Software',
	'pkts_tx': 'Packets Tx',
	'pkts_rx': 'Packets Rx',
	'bytes_tx': 'Bytes Tx',
	'bytes_rx': 'Bytes Rx',
	'bytes_rates': 'Tx/Rx bytes/s',
	'obuf_q': 'OutQ',
	'heard_count': 'MsgRcpts',
	'filter': 'Filter'
};

/* applications which typically have a port 14501 status port - can be linked */
var linkable = {
	'aprsc': 1,
	'aprsd': 1,
	'javAPRSSrvr': 1
};

/* clients per fd, to support onclick actions within client/uplink/peer tables */
var fd_clients = {};
var rx_err_codes = []; /* an array of rx err field codes */

/* tooltip action for rx errors counters */
function event_click_coordinates(e)
{
	var posx = 0;
	var posy = 0;
	
	if (e.pageX || e.pageY) {
		posx = e.pageX;
		posy = e.pageY;
	} else if (e.clientX || e.clientY) {
		posx = e.clientX + document.body.scrollLeft + document.documentElement.scrollLeft;
		posy = e.clientY + document.body.scrollTop + document.documentElement.scrollTop;
	}/* else {
		alert("event_click_coordinates failed!");
	}*/
	
	return [ posx, posy ];
}

var ttip_inside_element;
function ttip(e, elem, fd)
{
	var co = event_click_coordinates(e);
	
	// make note of element we're in
	ttip_inside_element = elem;
	
	//jsjam-keep: event_attach
	//$(elem).on('mouseout', ttip_hide);
	setTimeout(function() { ttip_show_maybe(elem, function() { return "contents"; }, co); }, 300);
	//deb("  added listener");
}

function rx_err_contents(fd)
{
	if (isUndefined(fd_clients[fd]))
		return 'No client on fd ' + fd;
		
	var er = fd_clients[fd]['rx_errs'];
	
	if (isUndefined(er))
		return 'No rx errors for client on fd ' + fd;
	
	var s = '<b>Receive drops: ' + fd_clients[fd]['pkts_dup'] + ' dupes, ' + fd_clients[fd]['pkts_ign'] + ' errors in ' + fd_clients[fd]['pkts_rx'] + '</b><br />';
	
	for (var i = 0; i < rx_err_codes.length; i++) {
		if (er[i] < 1)
			continue;
		s += ((rx_err_strings[rx_err_codes[i]]) ? rx_err_strings[rx_err_codes[i]] : rx_err_codes[i])
			+ ': ' + er[i] + '<br />';
	}
	
	return s;
}

function rx_err_popup(e, fd)
{
	cancel_events(e);
	
	if (isUndefined(fd_clients[fd]))
		return;
	
	var co = event_click_coordinates(e);
	
	ttip_show(function() { return rx_err_contents(fd); }, co);
	
	return false;
}

function cert_contents(fd)
{
	if (isUndefined(fd_clients[fd]))
		return 'No client on fd ' + fd;
		
	var subj = fd_clients[fd]['cert_subject'];
	var iss = fd_clients[fd]['cert_issuer'];
	
	if (isUndefined(subj) && isUndefined(iss))
		return 'Client on fd ' + fd + ' has no certificate';
	
	var s = '';
	
	if (!isUndefined(subj))
		s += '<b>Subject:</b><br />' + htmlent(subj) + '<br />';
	if (!isUndefined(iss))
		s += '<b>Issuer:</b><br />' + htmlent(iss) + '<br />';
		
	return s;
}

function cert_popup(e, fd)
{
	cancel_events(e);
	
	if (isUndefined(fd_clients[fd]))
		return;
	
	var co = event_click_coordinates(e);
	
	ttip_show(function() { return cert_contents(fd); }, co);
	
	return false;
}

function ttip_show_maybe(elem, cb, co)
{
	if (ttip_inside_element == elem)
		ttip_show(cb, co);
}

function ttip_show(cb, co)
{
	var element = $('#ttip');
	element.hide();
	
	var ttip_size = 300;
	if (co[0] > ttip_size + 40)
		co[0] -= ttip_size + 40; // position on left of event
	else
		co[0] += 40; // position on right of event
	
	element.html("<span>" + cb() + "</span>");
	element.css({ 'left': co[0] + 'px', 'top': co[1] + 'px'}).show('fast');
}

function ttip_hide()
{
	$('#ttip').hide('fast');
	ttip_inside_element = null;
}

/* render a clients array (also peers, uplinks, and listeners) */
function render_clients(element, d, cols)
{
	var s = '<table><tr>';
	for (var k in cols) {
		s += '<th>' + htmlent(cols[k]) + '</th>';
	}
	s += '</tr>';
	
	for (var ci in d) {
		s += '<tr>';
		var c = d[ci];
		
		if (isUndefined(c['fd']) || c['fd'] < 0)
			c['fd'] = Math.random() * -1000000;
		
		fd_clients[c['fd']] = c;
		c['addr_rem_shown'] = c['addr_rem'];
		
		if (c['udp_downstream']) { 
			if (c['mode'] == 'peer')
				c['addr_rem_shown'] += ' UDP';
			else
				c['addr_rem_shown'] += ' +UDP';
		}
		
		if (linkable[c['app_name']] || c['mode'] == 'peer')
			c['addr_rem_shown'] = '<a href="http://' + server_status_host(c) + '/">' + htmlent(c['addr_rem_shown']) + '</a>';
		
		if (c['app_name'] && c['app_version'])
			c['show_app_name'] = c['app_name'] + ' ' + c['app_version'];
		else
			c['show_app_name'] = c['app_name'];
		
		for (var k in cols) {
			s += '<td>';
			if (val_convert_c[k])
				s += val_convert_c[k](c, k);
			else if (val_convert[k])
				s += val_convert[k](c[k]);
			else
				s += htmlent(c[k]);
			s += '</td>';
		}
		
		s += '</tr>';
	}
	s += '</table>';
	
	element.html(s);
	return;
}

var graph_selected = '';

function render_block(graph_tree, element, d)
{
	var s = '<table>';
	
	for (var k in d) {
		if (!key_translate[k])
			continue;
			
		var o;
		if (typeof(d[k]) == 'object') {
			for (var i in d[k])
				d[k][i] = htmlent(d[k][i]);
			o = '<td class="ar">' + d[k].join('</td><td class="ar">') + '</td>';
		} else {
			if (val_convert[k])
				o = val_convert[k](d[k]);
			else
				o = htmlent(d[k]);
			
			o = '<td class="ar">' + o + '</td>';
		}
		
		if (graph_tree) {
			var id = graph_tree + '.' + k;
			var cl = (graph_selected == id) ? 'grtd grtd_sel' : 'grtd';
			s += '<tr><td class="' + cl + '" id="' + id.replace('.', '_') + '" onclick="gr_switch(\'' + id + '\')">' + htmlent(key_translate[k]) + '</td>' + o + '</tr>';
		} else {
			s += '<tr><td>' + htmlent(key_translate[k]) + '</td>' + o + '</tr>';
		}
	}
	s += '</table>';
	
	element.html(s);
	return;
}

var mem_rows = {
	'pbuf_small': 'Small pbufs',
	'pbuf_medium': 'Medium pbufs',
	'pbuf_large': 'Large pbufs',
	'historydb': 'Position history',
	'dupecheck': 'Dupecheck DB',
	'client': 'Clients',
	'client_heard': 'Client MsgRcpts',
	'client_courtesy': 'Client CourtesySrcs',
	'filter': 'Filter entries',
	'filter_wx': 'Filter WX stations',
	'filter_entrycall': 'Filter entrycalls'
};

var mem_cols = {
	'': 'Type',
	'_cell_size_aligned': 'Cell size',
	'_cells_used': 'Cells used',
	'_cells_free': 'Cells free',
	'_used_bytes': 'Bytes used',
	'_allocated_bytes': 'Bytes allocated',
	'_blocks': 'Blocks allocated'
};

function render_memory(element, d)
{
	var s = '<tr>';
	for (var k in mem_cols) {
		s += '<th>' + htmlent(mem_cols[k]) + '</th>';
	}
	s += '</tr>';
	
	for (var t in mem_rows) {
		if (isUndefined(d[t + '_cells_used']))
			continue;
		
		s += '<tr>';
		for (var k in mem_cols) {
			if (k == '')
				s += '<td>' + htmlent(mem_rows[t]) + '</td>';
			else {
				var rk = t + k;
				if (k == '_blocks' && !isUndefined(d[rk]))
					d[rk] += '/' + d[t + '_blocks_max'];
				s += '<td>' + htmlent(d[rk]) + '</th>';
			}
		}
		s += '</tr>';
	}
	
	$(element).html(s);
}

var tick_now;

var rate_cache = {};
function calc_rate(key, value, no_s)
{
	var rate = '';
	if (rate_cache[key]) {
		// can calculate current rate
		var t_dif = tick_now - rate_cache[key][0];
		var val_dif = value - rate_cache[key][1];
		rate = val_dif / t_dif;
		var prefix = '';
		if (rate < 0) {
			rate *= -1;
			prefix = '-';
		}
		
		if (rate >= 10)
			rate = rate.toFixed(0);
		else if (rate >= 1)
			rate = rate.toFixed(1);
		else if (rate > 0)
			rate = rate.toFixed(2);
		else if (rate == 0)
			rate = '0';
		else
			rate = rate.toFixed(2);
		if (!no_s)
			rate += '/s';
		rate = prefix + rate;
	}
	
	rate_cache[key] = [tick_now, value];
	return [ value, rate ];
}

var totals_keys = [
	'clients', 'connects',
	'tcp_bytes_tx', 'tcp_bytes_rx', 'tcp_pkts_tx', 'tcp_pkts_rx',
	'udp_bytes_tx', 'udp_bytes_rx', 'udp_pkts_tx', 'udp_pkts_rx',
	'sctp_bytes_tx', 'sctp_bytes_rx', 'sctp_pkts_tx', 'sctp_pkts_rx'
];

var alarms_visible = 0;

function render_alarms(alarms)
{
	var s = '';
	for (var i = 0; i < alarms.length; i++) {
		a = alarms[i];
		//console.log("alarm " + i + ": " + a['err']);
		if (a['set'] != 1)
			continue;
		s += '<div class="msg_e">'
			+ ((alarm_strings[a['err']]) ? alarm_strings[a['err']] : a['err'])
			+ ' See server log for details.</div>';
	}
	
	if (s == "") {
		if (alarms_visible) {
			alarms_visible = 0;
			alarm_div.hide('slow', function() { alarm_div.empty(); });
			return;
		}
		return;
	}
	
	alarm_div.html(s);
	
	if (!alarms_visible) {
		alarm_div.show('fast');
		alarms_visible = 1;
	}
}

var options_s;

function render(d)
{
	if (d['status_options'] != options_s) {
		options_s = d['status_options'];
		parse_options(options_s);
	}
	if (d['server'] && d['server']['tick_now']) {
		var s = d['server'];
		
		if (s['server_id']) {
			document.title = htmlent(s['server_id']) + ' aprsc status';
			$('#serverid').html(htmlent(s['server_id']));
		}
		
		if (s['time_now'])
			$('#upt').html(' at ' + timestr(s['time_now']));
		
		if ((!isUndefined(s['software'])) && !isUndefined(s['software_version']))
			s['software'] = s['software'] + ' ' + s['software_version'];
		
		render_block(0, server_table, s);
	} else {
		return;
	}
	
	tick_now = d['server']['tick_now'];
	rx_err_codes = d['rx_errs'];
	
	if (d['alarms']) {
		render_alarms(d['alarms']);
	} else {
		if (alarms_visible) {
			alarms_visible = 0;
			alarm_div.hide('slow', function() { alarm_div.empty(); });
		}
	}
	
	if (d['dupecheck']) {
		var u = d['dupecheck'];
		u['dupes_dropped'] = calc_rate('dupecheck.dupes_dropped', u['dupes_dropped']);
		u['uniques_out'] = calc_rate('dupecheck.uniques_out', u['uniques_out']);
		render_block('dupecheck', dupecheck_table, u);
		if (u['variations'])
			render_block(0, dupecheck_more_table, u['variations']);
	}
	
	if (d['totals']) {
		var u = d['totals'];
		for (var i in totals_keys) {
			if (u[totals_keys[i]] !== undefined)
				u[totals_keys[i]] = calc_rate('totals.' + totals_keys[i], u[totals_keys[i]]);
		}
		render_block('totals', totals_table, u);
	}
	
	fd_clients = {};
	
	if (d['listeners'])
		render_clients(listeners_table, d['listeners'], listener_cols);
	
	if (d['uplinks'] && d['uplinks'].length > 0) {
		render_clients(uplinks_table, d['uplinks'], uplink_cols);
		$('#uplinks_d').show();
	} else {
		$('#uplinks_d').hide();
	}
	
	if (d['peers'] && d['peers'].length > 0) {
		render_clients(peers_table, d['peers'], peer_cols);
		$('#peers_d').show();
	} else {
		$('#peers_d').hide();
	}
		
	if (d['clients'])
		render_clients(clients_table, d['clients'], client_cols);
		
	if (d['memory'])
		render_memory(memory_table, d['memory']);
}


var next_req_timer;

function schedule_update()
{
	if (next_req_timer)
		clearTimeout(next_req_timer);
		
	next_req_timer = setTimeout(update_status, 10000);
}

var current_status;

function update_success(data)
{
	if (next_req_timer) {
		clearTimeout(next_req_timer);
		next_req_timer = 0;
	}
	
	top_status();
	/* If this is the first successful status download, motd needs to be
	 * updated too.
	 */
	if (!current_status) {
		current_status = data;
		motd_check();
	} else {
		current_status = data;
	}
	render(data);
	schedule_update();
}

function update_status()
{
	if (next_req_timer) {
		clearTimeout(next_req_timer);
		next_req_timer = 0;
	}
	
	$.ajax({
		url: '/status.json',
		dataType: 'json',
		cache: false,
		timeout: 5000,
		error: function(jqXHR, stat, errorThrown) {
			var msg = '';
			if (stat == 'timeout')
				msg = 'Status download timed out. Network or server down?';
			else if (stat == 'error')
				msg = 'Status download failed with an error. Network or server down?';
			else
				msg = 'Status download failed (' + stat + ').';
				
			if (errorThrown)
				msg += '<br />HTTP error: ' + htmlent(errorThrown);
				
			top_status('msg_e', msg);
			
			schedule_update();
		},
		success: update_success
	});
}

function graph_fill(cdata, opts)
{
	var vals = cdata['values'];
	var vl = vals.length;
	if (opts['div']) {
		var div = opts['div'];
		for (var i = 0; i < vl; i++)
			vals[i][1] = vals[i][1] / div;
	}
	for (var i = 0; i < vl; i++)
		vals[i][0] = vals[i][0] * 1000;
		
	var _d = [ { label: opts['label'], data: vals } ];
	
	var _x_opt = {
		mode: 'time'
	};
	
	var _y_opt = {
		min: 0
	};
	
	var _o = {
		grid: { hoverable: true, autoHighlight: false, minBorderMargin: 20 },
		legend: { position: 'nw' },
		colors: [ '#0000ff' ],
		xaxis: _x_opt,
		yaxis: _y_opt
	};
	
	$.plot($('#graph'), _d, _o);
}

var graphs = {
	'totals.clients': { 'label': 'Clients allocated' },
	'totals.connects': { 'label': 'Incoming connections/min' },
	'totals.tcp_bytes_rx': { 'label': 'Bytes/s Rx, TCP', 'div' : 60 },
	'totals.tcp_bytes_tx': { 'label': 'Bytes/s Tx, TCP', 'div' : 60 },
	'totals.udp_bytes_rx': { 'label': 'Bytes/s Rx, UDP', 'div' : 60 },
	'totals.udp_bytes_tx': { 'label': 'Bytes/s Tx, UDP', 'div' : 60 },
	'totals.sctp_bytes_rx': { 'label': 'Bytes/s Rx, SCTP', 'div' : 60 },
	'totals.sctp_bytes_tx': { 'label': 'Bytes/s Tx, SCTP', 'div' : 60 },
	'totals.tcp_pkts_rx': { 'label': 'APRS-IS packets/s Rx, TCP', 'div' : 60 },
	'totals.tcp_pkts_tx': { 'label': 'APRS-IS packets/s Tx, TCP', 'div' : 60 },
	'totals.udp_pkts_rx': { 'label': 'APRS-IS packets/s Rx, UDP', 'div' : 60 },
	'totals.udp_pkts_tx': { 'label': 'APRS-IS packets/s Tx, UDP', 'div' : 60 },
	'totals.sctp_pkts_rx': { 'label': 'APRS-IS packets/s Rx, SCTP', 'div' : 60 },
	'totals.sctp_pkts_tx': { 'label': 'APRS-IS packets/s Tx, SCTP', 'div' : 60 },
	'dupecheck.dupes_dropped': { 'label': 'Duplicate packets dropped/s', 'div' : 60 },
	'dupecheck.uniques_out': { 'label': 'Unique packets/s', 'div' : 60 }
};

var graph_timer;

function load_graph_success(data)
{
	top_status();
	var d = graphs[this.k];
	graph_fill(data, d);
	graph_timer = setTimeout(load_graph, 60000);
}

function load_graph_error(jqXHR, stat, errorThrown)
{
	var msg = 'Graph data download failed (' + stat + '). Server or network down?';
	
	if (errorThrown)
		msg += '<br />HTTP error: ' + htmlent(errorThrown);
	
	top_status('msg_e', msg);
	
	graph_timer = setTimeout(load_graph, 60000);
}

function load_graph()
{
	var k = graph_selected;
	
	if (graph_timer) { 
		clearTimeout(graph_timer);
		graph_timer = 0;
	}
	
	$('.grtd').removeClass('grtd_sel');
	$('#' + k.replace('.', '_')).addClass('grtd_sel');
	
	$.ajax({
		url: '/counterdata?' + k,
		dataType: 'json',
		timeout: 5000,
		context: { 'k': k },
		success: load_graph_success,
		error: load_graph_error
	});
}

function gr_switch(id)
{
	graph_selected = id;
	load_graph();
}

var motd_last;

function motd_hide()
{
	$('#motd').hide('slow');
	motd_last = '';
}

function motd_success(data)
{
	if (data) {
		/* don't refill and re-animate if it doesn't change. */
		if (data == motd_last)
			return;
		motd_last = data;
		$('#motd').html(data);
		$('#motd').show('fast');
	} else {
		motd_hide();
	}
}

function motd_check()
{
	if (current_status && current_status['motd']) {
		$.ajax({
			url: current_status['motd'],
			dataType: 'html',
			timeout: 30000,
			success: motd_success,
			error: motd_hide
		});
	} else {
		motd_hide();
	}
		
	setTimeout(motd_check, 61000);
}

function toggle(id)
{
	$('#' + id + '_show').toggle(100);
	$('#' + id + '_hide').toggle(100);
	$('#' + id).toggle(200);
}

function init()
{
	listeners_table = $('#listeners');
	uplinks_table = $('#uplinks');
	peers_table = $('#peers');
	clients_table = $('#clients');
	memory_table = $('#memory');
	dupecheck_table = $('#dupecheck');
	dupecheck_more_table = $('#dupecheck_more');
	totals_table = $('#totals');
	server_table = $('#server');
	alarm_div = $('#alarms');
	
	update_status();
	gr_switch('totals.tcp_bytes_rx');
}

//-->
