package functional

import (
	"strings"
	"testing"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// Duplicate detection. Port of t/10dupecheck.t.
//
// The cases share one server and depend on its dupe cache state, so they run
// in order and must not be made parallel or reordered.
func TestDupecheck(t *testing.T) {
	harness.Parallel(t)
	s := harness.Start(t, "basic")

	const login = "N5CAL-1"
	tx := isclient.Connect(t, harness.Addr(t, 55580), login)
	rx := isclient.Connect(t, harness.Addr(t, 55152), "N5CAL-2")

	const data = "foo"

	long := "SRC>LONG,qAR," + login + ":long "
	long += strings.Repeat("a", 509-len(long))

	steps := []isclient.Step{
		// First, a non-APRS packet goes through.
		pass("first", "SRC>DST,qAR,"+login+":"+data,
			"SRC>DST,qAR,"+login+":"+data),
		// The same packet again immediately is dropped.
		dropDupe("exact dupe", "SRC>DST,qAR,"+login+":"+data,
			"SRC>DST:dummy"),
		// Source callsign is case sensitive.
		pass("lower-case source", "src>DST,qAR,"+login+":"+data,
			"src>DST,qAR,"+login+":"+data),
		// A different digi path is still a dupe.
		dropDupe("different digi path", "SRC>DST,DIGI1*,qAR,"+login+":"+data,
			"SRC>DST:dummy"),
		// A different Q construct is still a dupe.
		dropDupe("different q construct", "SRC>DST,"+login+",I:"+data,
			"SRC>DST:dummy"),
		// A different dstcall SSID is still a dupe.
		dropDupe("different dstcall ssid", "SRC>DST-2,"+login+",I:"+data,
			"SRC>DST:dummy2"),
		// Additional trailing whitespace is a different packet, passed as is
		// (with the q construct added).
		pass("trailing whitespace", "SRC>DST,"+login+",I:"+data+"  ",
			"SRC>DST,qAR,"+login+":"+data+"  "),
		// A different destination call is a different packet.
		pass("different dstcall", "SRC>DST2,DIGI1*,qAR,"+login+":"+data,
			"SRC>DST2,DIGI1*,qAR,"+login+":"+data),

		// A packet with trailing whitespace, then the trimmed version is a dupe.
		pass("whitespace original", "SRC>DST2,DIGI1*,qAR,"+login+":xxx ",
			"SRC>DST2,DIGI1*,qAR,"+login+":xxx "),
		dropDupe("whitespace trimmed", "SRC>DST2,DIGI1*,qAR,"+login+":xxx",
			"SRC>DST:dummy3"),

		// 8-bit data: variants with the high bytes removed, replaced with
		// spaces, or with the 8th bit stripped are dupes.
		pass("8bit original", "SRC>DST2,qAR,"+login+":latin1 \xC5\xC4\xD6 skandit",
			"SRC>DST2,qAR,"+login+":latin1 \xC5\xC4\xD6 skandit"),
		dropDupe("8bit removed", "SRC>DST2,qAR,"+login+":latin1  skandit",
			"SRC>DST:latin1 dummy1"),
		dropDupe("8bit replaced with spaces", "SRC>DST2,qAR,"+login+":latin1     skandit",
			"SRC>DST:latin1 dummy2"),
		dropDupe("8bit trimmed to 7 bits", "SRC>DST2,qAR,"+login+":latin1 \x45\x44\x56 skandit",
			"SRC>DST:latin1 dummy3"),

		// Control characters (< 0x20).
		pass("low original", "SRC>DST3,qAR,"+login+":lowdata \x1C\x1D\x1Fend",
			"SRC>DST3,qAR,"+login+":lowdata \x1C\x1D\x1Fend"),
		dropDupe("low removed", "SRC>DST3,qAR,"+login+":lowdata end",
			"SRC>DST:low dummy1"),
		dropDupe("low replaced with spaces", "SRC>DST3,qAR,"+login+":lowdata    end",
			"SRC>DST:low dummy2"),

		// DEL characters (0x7f).
		pass("del original", "SRC>DST3,qAR,"+login+":del \x7F\x7Fend",
			"SRC>DST3,qAR,"+login+":del \x7F\x7Fend"),
		dropDupe("del removed", "SRC>DST3,qAR,"+login+":del end",
			"SRC>DST:del dummy1"),
		dropDupe("del replaced with spaces", "SRC>DST3,qAR,"+login+":del   end",
			"SRC>DST:del dummy2"),

		// A really long packet, and a duplicate copy of it.
		pass("long original", long, long),
		dropDupe("long dupe", long, "SRC>LONG:long dummy"),
	}

	isclient.RunSteps(t, tx, rx, steps)

	if err := rx.Close(); err != nil {
		t.Errorf("failed to disconnect rx: %v", err)
	}
	if err := tx.Close(); err != nil {
		t.Errorf("failed to disconnect tx: %v", err)
	}

	s.Stop()
}
