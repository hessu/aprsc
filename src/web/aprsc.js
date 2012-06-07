<!--

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

function client_bytes_rates(c, k)
{
	var ckey = c['addr_rem'] + ':' + k;
	var tx = calc_rate(ckey + ':tx', c['bytes_tx'], 1);
	var rx = calc_rate(ckey + ':rx', c['bytes_rx'], 1);
	if (!isUndefined(tx[1]) && tx[1] !== '')
		return tx[1] + ' / ' + rx[1];
	return '';
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
	'uniques_out': 'Unique packets seen'
};

var val_convert_c = {
	'bytes_rates': client_bytes_rates
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

var uplink_cols = {
	'username': 'Server ID',
	'addr_rem': 'Address',
	't_connect': 'Connected',
	'since_connect': 'Up',
	'since_last_read': 'Last in',
	'show_app_name': 'Software',
	'pkts_tx': 'Packets Tx',
	'pkts_rx': 'Packets Rx',
	'bytes_tx': 'Bytes Tx',
	'bytes_rx': 'Bytes Rx',
	'bytes_rates': 'Tx/Rx Bytes/s',
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
	'bytes_rates': 'Tx/Rx Bytes/s',
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
	$(element).empty();
	
	var s = '<tr>';
	for (var k in cols) {
		s += '<th>' + htmlent(cols[k]) + '</th>';
	}
	s += '</tr>';
	$(element).append(s);
	
	for (var ci in d) {
		s = '<tr>';
		var c = d[ci];
		
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
		$(element).append(s);
	}
}

function render_block(element, d)
{
	$(element).empty();
	
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
		
		$(element).append('<tr><td>' + htmlent(key_translate[k]) + '</td>' + o + '</tr>');
	}
}

var mem_rows = {
	'pbuf_small': 'Small pbufs',
	'pbuf_medium': 'Medium pbufs',
	'pbuf_large': 'Large pbufs',
	'historydb': 'Position history',
	'dupecheck': 'Dupecheck DB',
	'filter': 'Filter entries',
	'filter_wx': 'Filter WX stations',
	'filter_entrycall': 'Filter entrycalls',
	'client_heard': 'Client MsgRcpts',
	'client_courtesy': 'Client CourtesySrcs'
};

var mem_cols = {
	'': 'Type',
	'_cell_size_aligned': 'Cell size',
	'_cells_used': 'Cells used',
	'_cells_free': 'Cells free',
	'_used_bytes': 'Bytes used',
	'_allocated_bytes': 'Allocated bytes',
	'_blocks': 'Allocated blocks'
};

function render_memory(element, d)
{
	$(element).empty();
	
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
	$(element).append(s);
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
		if (rate >= 10)
			rate = rate.toFixed(0);
		else if (rate >= 1)
			rate = rate.toFixed(1);
		else if (rate >= 0.1)
			rate = rate.toFixed(2);
		else if (rate == 0)
			rate = '0';
		else
			rate = rate.toFixed(3);
		if (!no_s)
			rate += '/s';
	}
	
	rate_cache[key] = [t_now, value];
	return [ value, rate ];
}

function render(d)
{
	if (d['server'] && d['server']['t_now']) {
		var s = d['server'];
		
		if (s['server_id'])
			document.title = htmlent(s['server_id']) + ' aprsc status';
			
		if (s['t_now'])
			$('#upt').html(' at ' + timestr(s['t_now']));
		
		if ((!isUndefined(s['software'])) && !isUndefined(s['software_version']))
			s['software'] = s['software'] + ' ' + s['software_version'];
			
		render_block('#server', s);
	} else {
		return;
	}
	
	t_now = d['server']['t_now'];
	
	if (d['dupecheck']) {
		var u = d['dupecheck'];
		u['dupes_dropped'] = calc_rate('dupecheck.dupes_dropped', u['dupes_dropped']);
		u['uniques_out'] = calc_rate('dupecheck.uniques_out', u['uniques_out']);
		render_block('#dupecheck', u);
	}
	
	if (d['uplinks'])
		render_clients('#uplinks', d['uplinks'], uplink_cols);
		
	if (d['clients'])
		render_clients('#clients', d['clients'], client_cols);
		
	if (d['memory'])
		render_memory('#memory', d['memory']);
}

function update_status()
{
	$.ajax({
		url: '/status.json',
		dataType: 'json',
		cache: false,
		error: function() {
			setTimeout(function() { update_status(); }, 30000);
		},
		success: function(data) {
			render(data);
			setTimeout(function() { update_status(); }, 10000);
		}
	});
}

update_status();

//-->
