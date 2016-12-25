<!--

// add additional language codes here, in the end of the list:
var lang_list = ['en', 'fi'];
// and, if necessary, add one new line in the beginning here,
// for dialect mapping (en_US, en_GB to en):
var lang_map = {
	'en_*': 'en',
	'fi_*': 'fi',
	'*': 'en' // DO NOT remove or change the default mapping to 'en'
};

function isUndefined(v)
{
	var undef;
	return v === undef;
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
	
	if (isUndefined(i))
		return '';
	
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



/* filters */

/* return only port of an addr:port string */
function onlyport(s)
{
	return s.substr(s.lastIndexOf(':') + 1);
}

function ratestr(rate)
{
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
		
	return prefix + rate;
}

function array_to_dict_by_id(arr)
{
	var dict = {};
	
	for (var c in arr) {
		var cl = arr[c];
		dict[cl['id']] = cl;
	}
	
	return dict;
}

var keys_totals = [
	'clients', 'connects',
	'tcp_bytes_tx', 'tcp_bytes_rx', 'tcp_pkts_tx', 'tcp_pkts_rx',
	'udp_bytes_tx', 'udp_bytes_rx', 'udp_pkts_tx', 'udp_pkts_rx',
	'sctp_bytes_tx', 'sctp_bytes_rx', 'sctp_pkts_tx', 'sctp_pkts_rx'
];

var keys_dupecheck = [ 'dupes_dropped', 'uniques_out' ];
var keys_dupecheck_variations = [
	'exact', 'space_trim', '8bit_strip', '8bit_clear', '8bit_spaced',
	'low_strip', 'low_spaced', 'del_strip', 'del_spaced' ];

var cols_listener = [
	'proto', 'addr', 'name', 'clients', 'clients_peak', 'clients_max',
	'connects', 'connects_rates',
	'pkts_tx', 'pkts_rx', 'bytes_tx', 'bytes_rx', 'bytes_rates'
	];

var cols_uplinks = [
	'server_id', 'addr', 'mode', 't_connect', 'since_connect', 'since_last_read',
	'software',
	'pkts_tx', 'pkts_rx', 'bytes_tx', 'bytes_rx', 'bytes_rates',
	'obuf_q'
	];

var cols_peers = [
	'server_id', 'addr', 'since_last_read',
	'pkts_tx', 'pkts_rx', 'bytes_tx', 'bytes_rx', 'bytes_rates',
	'obuf_q'
	];

var cols_clients = [
	'port', 'username', 'addr', 'verified',
	'since_connect', 'since_last_read', 'software',
	'pkts_tx', 'pkts_rx', 'bytes_tx', 'bytes_rx', 'bytes_rates',
	'obuf_q',
	'heard_count', 'filter'
	];

var rows_mem = {
	'pbuf_small': 'Small pbufs',
	'pbuf_medium': 'Medium pbufs',
	'pbuf_large': 'Large pbufs',
	'historydb': 'Position history',
	'dupecheck': 'Dupecheck DB',
	'client': 'Clients',
	'client_heard': 'Client MsgRcpts',
	'filter': 'Filter entries',
	'filter_wx': 'Filter WX stations',
	'filter_entrycall': 'Filter entrycalls'
};

/* applications which typically have a port 14501 status port - can be linked */
var linkable = {
	'aprsc': 1,
	'aprsd': 1,
	'javAPRSSrvr': 1
};

var graphs_available = {
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


var app = angular.module('aprsc', [ 'pascalprecht.translate', 'graph', 'ngDialog' ]).
	config(function($translateProvider) {
		console.log('aprsc module config');
		
		$translateProvider.translations('en', {
			APRSC_STATUS_TITLE: '{{server_id}} aprsc status',
			
			SERVER_TITLE: 'Server',
			SERVER_SERVER_ID: 'Server ID',
			SERVER_SERVER_ADMIN: 'Server admin',
			SERVER_SERVER_EMAIL: 'Admin email',
			SERVER_SOFTWARE: 'Software',
			SERVER_SOFTWARE_FEATURES: 'Software features',
			SERVER_UPTIME: 'Uptime',
			SERVER_STARTED: 'Server started',
			SERVER_OS: 'Operating system',
			
			TOTALS_TITLE: 'Totals',
			TOTALS_clients: 'Clients',
			TOTALS_connects: 'Connects',
			TOTALS_tcp_pkts_tx: 'Packets Tx TCP',
			TOTALS_tcp_pkts_rx: 'Packets Rx TCP',
			TOTALS_tcp_bytes_tx: 'Bytes Tx TCP',
			TOTALS_tcp_bytes_rx: 'Bytes Rx TCP',
			TOTALS_udp_pkts_tx: 'Packets Tx UDP',
			TOTALS_udp_pkts_rx: 'Packets Rx UDP',
			TOTALS_udp_bytes_tx: 'Bytes Tx UDP',
			TOTALS_udp_bytes_rx: 'Bytes Rx UDP',
			TOTALS_sctp_pkts_tx: 'Packets Tx SCTP',
			TOTALS_sctp_pkts_rx: 'Packets Rx SCTP',
			TOTALS_sctp_bytes_tx: 'Bytes Tx SCTP',
			TOTALS_sctp_bytes_rx: 'Bytes Rx SCTP',
			
			DUPES_TITLE: 'Duplicate filter',
			DUPES_dupes_dropped: 'Duplicate packets dropped',
			DUPES_uniques_out: 'Unique packets seen',
			
			DUPES_exact: 'Exact duplicates',
			DUPES_space_trim: 'Dupes with spaces trimmed from end',
			DUPES_8bit_strip: 'Dupes with 8-bit bytes stripped out',
			DUPES_8bit_clear: 'Dupes with 8th bit set to 0',
			DUPES_8bit_spaced: 'Dupes with 8-bit bytes replaced with spaces',
			DUPES_low_strip: 'Dupes with low bytes stripped out',
			DUPES_low_spaced: 'Dupes with low bytes replaced with spaces',
			DUPES_del_strip: 'Dupes with DEL bytes stripped out',
			DUPES_del_spaced: 'Dupes with DEL bytes replaced with spaces',
			
			LISTENERS_TITLE: 'Port listeners',
			UPLINKS_TITLE: 'Uplinks',
			PEERS_TITLE: 'Peers',
			CLIENTS_TITLE: 'Clients',
			
			MEM_TITLE: 'Memory',
			MEM_TH_TYPE: 'Type',
			MEM_TH_cell_size: 'Cell size',
			MEM_TH_cells_used: 'Cells used',
			MEM_TH_cells_free: 'Cells free',
			MEM_TH_used_bytes: 'Bytes used',
			MEM_TH_allocated_bytes: 'Bytes allocated',
			MEM_TH_blocks: 'Blocks allocated',
			
			TH_proto: 'Proto',
			TH_addr: 'Address',
			TH_name: 'Name',
			TH_clients: 'Clients',
			TH_clients_peak: 'Peak',
			TH_clients_max: 'Max',
			TH_connects: 'Connects',
			TH_connects_rates: 'Conn/s',
			TH_pkts_tx: 'Packets Tx',
			TH_pkts_rx: 'Packets Rx',
			TH_bytes_tx: 'Bytes Tx',
			TH_bytes_rx: 'Bytes Rx',
			TH_bytes_rates: 'Tx/Rx bytes/s',
			
			TH_username: 'Username',
			TH_server_id: 'Server ID',
			TH_mode: 'Mode',
			TH_t_connect: 'Connected',
			TH_since_connect: 'Up',
			TH_since_last_read: 'Last in',
			TH_software: 'Software',
			TH_obuf_q: 'OutQ',
			
			TH_port: 'Port',
			TH_verified: 'Verified',
			TH_heard_count: 'MsgRcpts',
			TH_filter: 'Filter',
			
			RXERR_DIALOG_TITLE: '{{ pkts_rx }} packets received',
			RXERR_DIALOG_MESSAGE: '{{ pkts_dup }} duplicates and {{ pkts_ign }} erroneus packets dropped.',
			
			RXERR_unknown: 'Unknown error',
			RXERR_no_colon: 'No colon (":") in packet',
			RXERR_no_dst: 'No ">" in packet to mark beginning of destination callsign',
			RXERR_no_path: 'No path found between source callsign and ":"',
			RXERR_inv_srccall: 'Invalid source callsign',
			RXERR_no_body: 'No packet body/data after ":"',
			RXERR_inv_dstcall: 'Invalid destination callsign',
			RXERR_disallow_unverified: 'Packet from unverified local client',
			RXERR_disallow_unverified_path: 'Packet from unverified client (TCPXX)',
			RXERR_path_nogate: 'Packet with NOGATE/RFONLY in path',
			RXERR_party_3rd_ip: '3rd-party packet gated TCPIP>RF>TCPIP',
			RXERR_party_3rd_inv: 'Invalid 3rd-party packet header',
			RXERR_general_query: 'General query',
			RXERR_aprsc_oom_pbuf: 'aprsc out of packet buffers',
			RXERR_aprsc_class_fail: 'aprsc failed to classify packet',
			RXERR_aprsc_q_bug: 'aprsc Q construct processing failed',
			RXERR_q_drop: 'Q construct algorithm dropped packet',
			RXERR_short_packet: 'Packet too short',
			RXERR_long_packet: 'Packet too long',
			RXERR_inv_path_call: 'Invalid callsign in path',
			RXERR_q_qax: 'qAX: Packet from unverified remote client',
			RXERR_q_qaz: 'qAZ construct',
			RXERR_q_path_mycall: 'My ServerID in Q path',
			RXERR_q_path_call_twice: 'Same callsign twice in the Q path',
			RXERR_q_path_login_not_last: 'Local client login found but not last in Q path',
			RXERR_q_path_call_is_local: 'Callsign in Q path is a local verified client',
			RXERR_q_path_call_inv: 'Invalid callsign in Q path',
			RXERR_q_qau_path_call_srccall: 'qAU callsign in path equals srccall',
			RXERR_q_newq_buffer_small: 'New Q construct too big',
			RXERR_q_nonval_multi_q_calls: 'Multiple callsigns in Q path from unverified client',
			RXERR_q_i_no_viacall: 'I path has no viacall',
			RXERR_q_disallow_protocol: 'Invalid protocol ID in Q construct',
			RXERR_inerr_empty: 'Empty packet',
			RXERR_disallow_srccall: 'Disallowed source callsign (N0CALL or such)',
			RXERR_disallow_dx: 'DX cluster packet',
			RXERR_disallow_msg_dst: 'Disallowed message recipient (javaMSG, JAVATITLE, USERLIST...)',
			
			ALARM_no_uplink: "Server does not have any uplink connections.",
			ALARM_packet_drop_hang: "Server has dropped packets due to forward time leaps or hangs caused by resource starvation.",
			ALARM_packet_drop_future: "Server has dropped packets due to backward time leaps.",
			
			CERT_DIALOG_TITLE: '{{ username }} authenticated using a certificate.',
			CERT_SUBJECT: 'Certificate subject:',
			CERT_ISSUER: 'Certificate issuer:'
			
		});
		
		$translateProvider.useUrlLoader("/strings");
		$translateProvider.useSanitizeValueStrategy('escape');
		$translateProvider.preferredLanguage('en');
		$translateProvider.registerAvailableLanguageKeys(lang_list, lang_map).determinePreferredLanguage();
	}).
	run(function() {
		console.log('aprsc module run');
	});

app.filter('duration', function() { return dur_str; });
app.filter('datetime', function() { return timestr; });
app.filter('ratestr', function() { return ratestr; });
app.filter('onlyport', function() { return onlyport; });

app.controller('aprscc', [ '$scope', '$http', 'graphs', 'ngDialog', '$sce', function($scope, $http, graphs, ngDialog, $sce) {
	console.log('aprsc init');
	
	$scope.setup = {
	    'keys_totals': keys_totals,
	    'keys_dupecheck': keys_dupecheck,
	    'keys_dupecheck_variations': keys_dupecheck_variations,
	    'cols_listener': cols_listener,
	    'cols_uplinks': cols_uplinks,
	    'cols_peers': cols_peers,
	    'cols_clients': cols_clients,
	    'rows_mem': rows_mem
	};
	
	/* graph zooming and switching */
	$scope.graphZoom = graphs.graph_zoom;
	$scope.graphSwitch = function(tree, key) { graphs.gr_switch(tree + '.' + key); };
	$scope.graphClass = function(tree, key) { return (graphs.graph_selected == tree + '.' + key) ? "bg-info" : ""; };
	
	/* client list styling helpers */
	$scope.linkable = function(s) { return linkable[s.app_name]; };
	
	$scope.server_status_href = function(s) {
		var h = s['addr_rem'];
		var p = h.lastIndexOf(':');
		if (h.lastIndexOf(']') > p || p == -1)
			return 'http://' + h + ':14501';
		return 'http://' + h.substr(0, p) + ':14501';
	};
	
	$scope.pkts_rx_class = function(c) {
		if (c.pkts_ign > 0 || c.pkts_dup > 0)
			return (c.pkts_ign / c.pkts_rx > 0.1) ? 'rxerr_red' : 'rxerr';
			
		return '';
	};
	
	$scope.pkts_rx_dialog = function (c) {
		$scope.sel_c = c;
		ngDialog.open({ template: 'pkts_rx_dialog', className: 'ngdialog-theme-plain', scope: $scope });
	};
	
	$scope.client_cert_dialog = function (c) {
		$scope.sel_c = c;
		ngDialog.open({ template: 'client_cert_dialog', className: 'ngdialog-theme-plain', scope: $scope });
	};
	
	/* set up sorting for client and peer lists */
	$scope.clients_sort = {
		column: 'since_connect',
		descending: false
	};
	
	$scope.peers_sort = {
		column: 'username',
		descending: false
	};
	
	$scope.sortIndicator = function(sort, column) {
		if (column == sort.column) {
			return 'glyphicon glyphicon-sort-by-attributes'
				+ ((sort.descending) ? '-alt' : '');
		}
		return '';
	};
	
	$scope.changeSorting = function(sort, column) {
		if (sort.column == column) {
			sort.descending = !sort.descending;
		} else {
			sort.column = column;
			sort.descending = false;
		}
	};
	
	var motd_last;

	var motd_hide = function() {
		$scope.motd = null;
		motd_last = '';
	};
	
	var motd_success = function(data) {
		if (data) {
			/* don't refill and re-animate if it doesn't change. */
			if (data == motd_last)
				return;
				
			motd_last = data;
			$scope.motd = $sce.trustAsHtml(data);
		} else {
			motd_hide();
		}
	};
	
	var motd_check = function(status) {
		if (status && status['motd']) {
			$http({ method: 'GET', url: status['motd']}).then(function successCallback(r) {
				console.log('motd received: ' + r.status);
				motd_success(r.data);
			});
		} else {
			motd_hide();
		}
	};
	
	/* status options parsing */
	var options_s;
	
	var parse_options = function(s) {
		if (!s) {
			$scope.options = {};
			return;
		}
		
		var o = {};
		var a = s.split(' ');
		for (var i = 0; i < a.length; i++) {
			var p = a[i].split('=');
			var c = p[0].toLowerCase();
			o[c] = p[1];
		}
		
		$scope.options = o;
	}
	
	/* Ajax updates */
	
	var full_load = function($scope, $http) {
		var config = {
			'timeout': 35000
		};
		
		$http({ method: 'GET', url: '/status.json', config: config}).then(function successCallback(r) {
			console.log('status.json received: ' + r.status);
			
			var d = r.data;
			
			if ($scope.status) {
				d.tick_dif = d.server.tick_now - $scope.status.server.tick_now;
				$scope.status['clients_id'] = array_to_dict_by_id($scope.status.clients);
				$scope.status['uplinks_id'] = array_to_dict_by_id($scope.status.uplinks);
				$scope.status['peers_id'] = array_to_dict_by_id($scope.status.peers);
				$scope.status['listeners_id'] = array_to_dict_by_id($scope.status.listeners);
			}
			
			$scope.status_prev = $scope.status;
			$scope.status = d;
			$scope.uierror = null;
			
			if (d['status_options'] != options_s) {
				options_s = d['status_options'];
				parse_options(options_s);
			}
			
			motd_check(d);
			
			setTimeout(function() { full_load($scope, $http); }, 10000);
			
		}, function errorCallback(r) {
			console.log('HTTP status.json fetch failed: ' + r.status);
			
			$scope.uierror = 'Server status download failed with an error. Network or server down?';
			
			setTimeout(function() { full_load($scope, $http); }, 10000);
		});
	};
	
	full_load($scope, $http);
	
	graphs.graph_setup($scope, graphs_available);

}]);

//-->
