<!--

var graph_module = angular.module('graph', [ ]).
	factory('graphs', function() {
		console.log("graphService setup");
		var instance = {};
		
		instance.graph_fill = function(cdata, opts) {
			var vals = cdata['values'];
			var vl = vals.length;
			if (opts['div']) {
				var div = opts['div'];
				for (var i = 0; i < vl; i++)
					vals[i][1] = vals[i][1] / div;
			}
			for (var i = 0; i < vl; i++)
				vals[i][0] = vals[i][0] * 1000;
			
			instance.graph_data = [ { label: opts['label'], data: vals } ];
			
			var _x_opt = {
				mode: 'time'
			};
			
			var _y_opt = {
				min: 0
			};
			
			instance.graph_opt = {
				grid: { hoverable: true, autoHighlight: false, minBorderMargin: 20 },
				legend: { position: 'nw' },
				colors: [ '#0000ff' ],
				xaxis: _x_opt,
				yaxis: _y_opt,
				selection: { mode: "x" }
			};
			
			$.plot($('#graph'), instance.graph_data, instance.graph_opt);
                }

		instance.schedule_graph = function(t) {
			if (instance.graph_timer)
				clearTimeout(instance.graph_timer);
			instance.graph_timer = setTimeout(instance.load_graph, t);
		};
		
		instance.load_graph_success = function(data) {
			var d = instance.graphs_available[this.k];
			instance.graph_fill(data, d);
			instance.schedule_graph(60000);
			$('#graph').trigger('plotunselected');
			instance.scope.graph_zoomed = false;
		};
		
		instance.load_graph_error = function(jqXHR, stat, errorThrown) {
			/*
			var msg = 'Graph data download failed (' + stat + '). Server or network down?';
			
			if (errorThrown)
				msg += '<br />HTTP error: ' + htmlent(errorThrown);
			
			instance.scope.uierror = msg_e;
			*/
			instance.schedule_graph(60000);
		};
		
		instance.load_graph = function($scope) {
			var k = instance.graph_selected;
			
			if (instance.graph_timer) { 
				clearTimeout(instance.graph_timer);
				instance.graph_timer = 0;
			}
			
			$('.grtd').removeClass('grtd_sel');
			$('#' + k.replace('.', '_')).addClass('grtd_sel');
			
			$.ajax({
				url: '/counterdata?' + k,
				dataType: 'json',
				timeout: 5000,
				context: { 'k': k, 'scope': $scope },
				success: instance.load_graph_success,
				error: instance.load_graph_error
			});
		};
		
		instance.gr_switch = function(id) {
			instance.graph_selected = id;
			instance.range_selected = false;
			instance.scope.graph_zoomed = false;
			$('#graph').trigger('plotunselected');
			instance.load_graph(instance.scope);
		};
		
		instance.graph_zoom = function(zoom_in) {
			if (instance.range_selected && zoom_in) {
				instance.scope.graph_zoomed = true;
				instance.graph_opt.xaxis.min = instance.range_selected.from;
				instance.graph_opt.xaxis.max = instance.range_selected.to;
			} else {
				instance.scope.graph_zoomed = false;
				instance.graph_opt.xaxis.min = null;
				instance.graph_opt.xaxis.max = null;
			}
			
			var p = $.plot($('#graph'), instance.graph_data, instance.graph_opt);
			
			if (instance.range_selected)
				p.setSelection({ xaxis: { from: instance.range_selected.from, to: instance.range_selected.to }});
		};


                instance.graph_setup = function($scope, graphs_available) {
                	instance.scope = $scope;
                	instance.graphs_available = graphs_available;
                        instance.gr_switch('totals.tcp_bytes_rx');
                        
                        $('#graph').bind('plotselected', function(event,ranges) {
                                var to = parseInt(ranges.xaxis.to.toFixed(0));
                                var from = parseInt(ranges.xaxis.from.toFixed(0));
                                instance.range_selected = {
                                        'from': from,
                                        'to': to
                                };
                                $('#g_zoom_in').removeAttr('disabled');
                                instance.schedule_graph(60000); /* delay next update */
                        });
                        
                        $('#graph').bind('plotunselected', function(event,ranges) {
                                instance.range_selected = undefined;
                                $('#g_zoom_in').attr("disabled", "disabled");
                        });
                };
		
		return instance;
	});

//-->
