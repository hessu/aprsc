<!--

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

function server_status_host(s)
{
	var h = s['addr_rem'];
	return h.substr(0, h.lastIndexOf(':')) + ':14501';
}

function addr_loc_port(s)
{
	return s.substr(s.lastIndexOf(':') + 1);
}

function username_link(s)
{
	return '<a href="http://aprs.fi/?call=' + s + '" target="_blank">' + htmlent(s) + "</a>";
}

function conv_verified(i)
{
	if (i)
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
	if (isUndefined(c['pkts_ign']))
		return c['pkts_rx'];
	
	var s = c['pkts_rx'] + '/' + c['pkts_ign'];
	
	if (c['pkts_ign'] / c['pkts_rx'] > 0.1)
		return '<span class="red">' + s + '</span>';

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

var listeners_table, uplinks_table, peers_table, clients_table, memory_table, dupecheck_table, totals_table;

var key_translate = {
	// server block
	'server_id': 'Server ID',
	'admin': 'Server admin',
	'software': 'Server software',
	'os': 'Operating system',
	't_started': 'Server started',
	'uptime': 'Uptime',
	
	// dupecheck block
	'dupes_dropped': 'Duplicate packets dropped',
	'uniques_out': 'Unique packets seen',
	
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
	'udp_bytes_rx': 'Bytes Rx UDP'
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
	'udp_bytes_rx': 'APRS-IS data received over UDP'
};

var val_convert_c = {
	'bytes_rates': client_bytes_rates,
	'pkts_rx': client_pkts_rx,
	'connects_rates': port_conn_rates
};

var val_convert = {
	't_started': timestr,
	'uptime': dur_str,
	't_connect': timestr,
	'since_connect': dur_str,
	'since_last_read': dur_str,
	'addr_rem': conv_none,
	'username': username_link,
	'addr_loc': addr_loc_port,
	'verified': conv_verified
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
	'addr_rem': 'Address',
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
	'addr_rem': 'Address',
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
	'addr_rem': 'Address',
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

var linkable = {
	'aprsc': 1,
	'aprsd': 1,
	'javAPRSSrvr': 1
};

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
		
		if (c['udp_downstream']) { 
			if (c['mode'] == 'peer')
				c['addr_rem'] += ' UDP';
			else
				c['addr_rem'] += ' +UDP';
		}
		
		if (linkable[c['app_name']])
			c['addr_rem'] = '<a href="http://' + server_status_host(c) + '/">' + htmlent(c['addr_rem']) + '</a>';
		
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

var t_now;

var rate_cache = {};
function calc_rate(key, value, no_s)
{
	var rate = '';
	if (rate_cache[key]) {
		// can calculate current rate
		var t_dif = t_now - rate_cache[key][0];
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
	
	rate_cache[key] = [t_now, value];
	return [ value, rate ];
}

var totals_keys = [
	'clients', 'connects',
	'tcp_bytes_tx', 'tcp_bytes_rx', 'tcp_pkts_tx', 'tcp_pkts_rx',
	'udp_bytes_tx', 'udp_bytes_rx', 'udp_pkts_tx', 'udp_pkts_rx'
];

function render(d)
{
	if (d['server'] && d['server']['t_now']) {
		var s = d['server'];
		
		if (s['server_id']) {
			document.title = htmlent(s['server_id']) + ' aprsc status';
			$('#serverid').html(htmlent(s['server_id']));
		}
		
		if (s['t_now'])
			$('#upt').html(' at ' + timestr(s['t_now']));
		
		if ((!isUndefined(s['software'])) && !isUndefined(s['software_version']))
			s['software'] = s['software'] + ' ' + s['software_version'];
			
		render_block(0, server_table, s);
	} else {
		return;
	}
	
	t_now = d['server']['t_now'];
	
	if (d['dupecheck']) {
		var u = d['dupecheck'];
		u['dupes_dropped'] = calc_rate('dupecheck.dupes_dropped', u['dupes_dropped']);
		u['uniques_out'] = calc_rate('dupecheck.uniques_out', u['uniques_out']);
		render_block('dupecheck', dupecheck_table, u);
	}
	
	if (d['totals']) {
		var u = d['totals'];
		for (var i in totals_keys) {
			u[totals_keys[i]] = calc_rate('totals.' + totals_keys[i], u[totals_keys[i]]);
		}
		render_block('totals', totals_table, u);
	}
	
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

function update_success(data)
{
	if (next_req_timer) {
		clearTimeout(next_req_timer);
		next_req_timer = 0;
	}
	
	top_status();
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
	'totals.tcp_pkts_rx': { 'label': 'APRS-IS packets/s Rx, TCP', 'div' : 60 },
	'totals.tcp_pkts_tx': { 'label': 'APRS-IS packets/s Tx, TCP', 'div' : 60 },
	'totals.udp_pkts_rx': { 'label': 'APRS-IS packets/s Rx, UDP', 'div' : 60 },
	'totals.udp_pkts_tx': { 'label': 'APRS-IS packets/s Tx, UDP', 'div' : 60 },
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

function init()
{
	listeners_table = $('#listeners');
	uplinks_table = $('#uplinks');
	peers_table = $('#peers');
	clients_table = $('#clients');
	memory_table = $('#memory');
	dupecheck_table = $('#dupecheck');
	totals_table = $('#totals');
	server_table = $('#server');
	
	update_status();
	gr_switch('totals.tcp_bytes_rx');
}

//-->
