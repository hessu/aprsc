package functional

import (
	"testing"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// APRS packet parsing and filtering: basic uncompressed, compressed and
// mic-e packets must pass a filter which matches their positions, and the p/
// prefix filter is tried out. Port of t/30parser-filter.t.
//
// TODO: add more filter test cases for the rest of the filters.
func TestParserFilter(t *testing.T) {
	s := harness.Start(t, "basic")

	const login = "N5CAL-1"
	tx := isclient.Connect(t, "localhost:55580", login)

	// First filter is for uncompressed packet, second for compressed,
	// third for mic-e, fourth for prefix filter test.
	// The first and last one also test upper-case letters as filter keys.
	rx := isclient.Connect(t, "localhost:55581", "N5CAL-2", isclient.WithFilter(
		"R/60.4752/25.0947/1 r/60.0520/24.5045/1 r/49.0/8.4/500 r/37.0887/-76.4585/100 "+
			"r/60.49966/-151.10416/5 r/35.0685/136.662833/2 P/OG/of3/N"))

	const phg = "7220"
	const srccall = "OH2RDP-1"
	const dstcall = "BEACON-15"

	isclient.RunSteps(t, tx, rx, []isclient.Step{
		// UNCOMPRESSED packet: the r/ range filter passes packets within the
		// range, and drops packets outside it.
		pass("uncompressed in range",
			srccall+">"+dstcall+",OH2RDG*,WIDE:!6028.51N/02505.68E#PHG"+phg+" should pass",
			srccall+">"+dstcall+",OH2RDG*,WIDE,qAS,"+login+":!6028.51N/02505.68E#PHG"+phg+" should pass"),
		drop("uncompressed out of range",
			srccall+">"+dstcall+",OH2RDG*,WIDE:!6258.51N/02505.68E#PHG"+phg+" should drop",
			srccall+">"+dstcall+",OH2RDG*,WIDE:!6028.51N/02505.68E#PHG"+phg+" should pass"),

		pass("compressed",
			srccall+">"+dstcall+":!I0-X;T_Wv&{-Aigate testing",
			srccall+">"+dstcall+",qAS,"+login+":!I0-X;T_Wv&{-Aigate testing"),

		pass("mic-e",
			"X3HF-9>S7PU3R:`h7Oq+F>/`\"3{}_",
			"X3HF-9>S7PU3R,qAS,"+login+":`h7Oq+F>/`\"3{}_"),
		// latitude: 47.93283333333 °, longitude: 12.93733333333 °
		passSame("mic-e 2", "OX8AAA>T7UU97,qAR,"+login+":`(T4l!u>/]\"83}="),
		// latitude: 60.49966666666667 °, longitude: -151.1041666666667 °
		passSame("mic-e 3", "KL7AN-14>V0RYYX,WIDE1-1,WIDE2-1,qAR,KL7IGZ-1:`O^5l \x1ck/]\"4%}="),
		// latitude: 35.0685 °, longitude: 136.6628333333333 °
		passSame("mic-e 4", "JN2ESN-14>SUPTQ1,WIDE1-1,qAR,JA2PIT-10:`@CioI}u/`\"4T}LPG Tanker_\""),

		// the P filter passes packets with the prefixes given
		passSame("prefix OG", "OG7LZB>"+dstcall+",OH2RDG*,WIDE,qAR,"+login+":>should pass, prefix"),
		passSame("prefix of3", "OF3LZB>"+dstcall+",OH2RDG*,WIDE,qAR,"+login+":>should pass, prefix"),
		passSame("prefix N", "N1CAL>"+dstcall+",OH2RDG*,WIDE,qAR,"+login+":>should pass, prefix"),
	})

	s.Stop()
}

// NMEA packet parsing and filtering: these packets must pass a filter which
// matches their positions. Port of t/31parser-nmea.t.
func TestParserNMEA(t *testing.T) {
	s := harness.Start(t, "basic")

	const login = "N5CAL-1"
	tx := isclient.Connect(t, "localhost:55580", login)
	rx := isclient.Connect(t, "localhost:55581", "N5CAL-2",
		isclient.WithFilter("r/-38.5452/-58.7366/1 s/->")) // GPRMC

	// The NMEA checksum must stay intact, so the dropped packets and their
	// helpers are sent literally.
	const car = "$GPRMC,212052,A,4609.1157,N,12258.8145,W,0.168,13.4,130909,17.9,E*6B"

	isclient.RunSteps(t, tx, rx, []isclient.Step{
		pass("passes by position",
			"OH1XYZ>GPSMW:$GPRMC,184649,A,3832.7107,S,05844.1957,W,0.000,0.0,130909,4.5,W*62",
			"OH1XYZ>GPSMW,qAS,"+login+":$GPRMC,184649,A,3832.7107,S,05844.1957,W,0.000,0.0,130909,4.5,W*62"),
		pass("passes by car symbol",
			"OH2XYZ>GPSMV:"+car,
			"OH2XYZ>GPSMV,qAS,"+login+":"+car),
		dropLiteral("wrong position and symbol",
			"OH3XYZ>GPSMW:"+car,
			"OH4XYZ>GPSMV:"+car),
		dropLiteral("invalid char in coordinates",
			"OH4XYZ>GPSMW:$GPRMO,182051.\xf000,A,6039.8655,N,01708.3799,E,20.07,243.41,070313,,,A*5A",
			"OH5XYZ>GPSMV:"+car),
	})

	s.Stop()
}

// Negative filters: allow a range, then drop using a buddy filter. Port of
// t/32filter-negative.t.
func TestFilterNegative(t *testing.T) {
	s := harness.Start(t, "basic")

	const login = "N5CAL-1"
	const srccall = "OH2RDP-1"
	const dstcall = "BEACON-15"

	tx := isclient.Connect(t, "localhost:55580", login)
	rx := isclient.Connect(t, "localhost:55581", "N5CAL-2",
		isclient.WithFilter("r/60.4752/25.0947/100 -b/D3NY"))

	// the r/ range filter passes packets within the range
	isclient.RunSteps(t, tx, rx, []isclient.Step{
		pass("in range",
			srccall+">"+dstcall+",OH2RDG*,WIDE:!6028.51N/02505.68E# should pass",
			srccall+">"+dstcall+",OH2RDG*,WIDE,qAS,"+login+":!6028.51N/02505.68E# should pass"),
	})

	s.Stop()
}
