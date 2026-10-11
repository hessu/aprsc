package functional

import (
	"testing"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// Third-party packet header parsing and filtering. Port of t/13thirdparty.t.
func TestThirdParty(t *testing.T) {
	s := harness.Start(t, "basic")

	const login = "N5CAL-1"
	tx := isclient.Connect(t, "localhost:55580", login)

	// First filter is for uncompressed packet, second for compressed,
	// third for mic-e, fourth for prefix filter test.
	// The first and last one also test upper-case letters as filter keys.
	rx := isclient.Connect(t, "localhost:55581", "N5CAL-2",
		isclient.WithFilter("r/60.228/24.8495/5 p/OG p/OI p/K b/BB7LZB"))

	same := func(name, packet string) isclient.Step {
		return pass(name, packet, packet)
	}

	steps := []isclient.Step{
		same("inner prefix matches p filter",
			"UU0AA>TEST,qAR,IGATE:}OG7LZB>DST,NET,GATE*:>should pass 3rd party inner prefix"),
		same("inner callsign matches b filter",
			"UU0AA>TEST,qAR,IGATE:}BB7LZB>DST,NET,GATE*:>should pass 3rd party inner prefix with b filter"),
		same("outer prefix matches p filter",
			"OI0AA>TEST,qAR,IGATE:}ZZ7LZB>DST,NET,GATE*:>should pass 3rd party outer prefix"),
		same("inner position matches coordinate filter",
			"UU1AA>TEST,qAR,IGATE:}OF7LZB>DST,NET,GATE:!6013.69NR02450.97E&"),
		same("nested 3rd party position matches coordinate filter",
			"UU1AA>TEST,qAR,IGATE:}OF7LZB>DST,NET,GATE:}OF7LZC>DST,NET2,GATE2:!6013.69NR02450.97E&"),
		same("deeply nested 3rd party position matches coordinate filter",
			"UU1AA>TEST,qAR,IGATE:}OF7LZB>DST,NET,GATE:}OF7LZC>DST,NET2,GATE2:}OF7LZD>DST,NET2,GATE2:}OF7LZE>DST,NET2,GATE2:!6013.69NR02450.97E&"),

		// Invalid 3rd-party header, does not have 2 hops
		same("invalid header, many hops, passes",
			"KG4LAA>APWW08,TCPIP*,qAS,n3wax:}KB3ONM>APK102,KV3B-1,WIDE1,WIDE2,KV3B-1,WIDE1,KG4LAA*::N3HEV-9  :ack26"),
		drop("invalid header, no 2 hops, no filter match",
			"KG4LAA>APWW08,TCPIP*,qAS,n3wax:}KB3ONM>APK102,KV3B-1::N3HEV-9  :ack26",
			"K2SRC>APRS,qAR,"+login+":>should pass 2"),

		// Only has the } marker, but is passed still (needs to have
		// }...>...: to be treated as 3rd-party)
		same("only the } marker",
			"K3SRC>APWW08,TCPIP*,qAR,IGA:}blah blah"),
		// Valid third-party header with a used digi
		same("valid header with a used digi",
			"K3SRC>APWW09,TCPIP*,qAR,IGA:}KB3ONM>APK102,KV3B-1,FOO,BAR*:should pass"),
	}

	isclient.RunSteps(t, tx, rx, steps)

	s.Stop()
}
