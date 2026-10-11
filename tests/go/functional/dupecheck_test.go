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
	s := harness.Start(t, "basic")

	const login = "N5CAL-1"
	tx := isclient.Connect(t, "localhost:55580", login)
	rx := isclient.Connect(t, "localhost:55152", "N5CAL-2")

	const data = "foo"

	long := "SRC>LONG,qAR," + login + ":long "
	long += strings.Repeat("a", 509-len(long))

	// pass: the packet must come out of rx as want.
	// drop: the packet must be dropped; only the helper packet comes out.
	type step struct {
		name string
		send string
		want string // pass: expected packet on rx
		drop string // drop: helper packet (without its unique token)
	}

	steps := []step{
		// First, a non-APRS packet goes through.
		{name: "first", send: "SRC>DST,qAR," + login + ":" + data,
			want: "SRC>DST,qAR," + login + ":" + data},
		// The same packet again immediately is dropped.
		{name: "exact dupe", send: "SRC>DST,qAR," + login + ":" + data,
			drop: "SRC>DST:dummy"},
		// Source callsign is case sensitive.
		{name: "lower-case source", send: "src>DST,qAR," + login + ":" + data,
			want: "src>DST,qAR," + login + ":" + data},
		// A different digi path is still a dupe.
		{name: "different digi path", send: "SRC>DST,DIGI1*,qAR," + login + ":" + data,
			drop: "SRC>DST:dummy"},
		// A different Q construct is still a dupe.
		{name: "different q construct", send: "SRC>DST," + login + ",I:" + data,
			drop: "SRC>DST:dummy"},
		// A different dstcall SSID is still a dupe.
		{name: "different dstcall ssid", send: "SRC>DST-2," + login + ",I:" + data,
			drop: "SRC>DST:dummy2"},
		// Additional trailing whitespace is a different packet, passed as is
		// (with the q construct added).
		{name: "trailing whitespace", send: "SRC>DST," + login + ",I:" + data + "  ",
			want: "SRC>DST,qAR," + login + ":" + data + "  "},
		// A different destination call is a different packet.
		{name: "different dstcall", send: "SRC>DST2,DIGI1*,qAR," + login + ":" + data,
			want: "SRC>DST2,DIGI1*,qAR," + login + ":" + data},

		// A packet with trailing whitespace, then the trimmed version is a dupe.
		{name: "whitespace original", send: "SRC>DST2,DIGI1*,qAR," + login + ":xxx ",
			want: "SRC>DST2,DIGI1*,qAR," + login + ":xxx "},
		{name: "whitespace trimmed", send: "SRC>DST2,DIGI1*,qAR," + login + ":xxx",
			drop: "SRC>DST:dummy3"},

		// 8-bit data: variants with the high bytes removed, replaced with
		// spaces, or with the 8th bit stripped are dupes.
		{name: "8bit original", send: "SRC>DST2,qAR," + login + ":latin1 \xC5\xC4\xD6 skandit",
			want: "SRC>DST2,qAR," + login + ":latin1 \xC5\xC4\xD6 skandit"},
		{name: "8bit removed", send: "SRC>DST2,qAR," + login + ":latin1  skandit",
			drop: "SRC>DST:latin1 dummy1"},
		{name: "8bit replaced with spaces", send: "SRC>DST2,qAR," + login + ":latin1     skandit",
			drop: "SRC>DST:latin1 dummy2"},
		{name: "8bit trimmed to 7 bits", send: "SRC>DST2,qAR," + login + ":latin1 \x45\x44\x56 skandit",
			drop: "SRC>DST:latin1 dummy3"},

		// Control characters (< 0x20).
		{name: "low original", send: "SRC>DST3,qAR," + login + ":lowdata \x1C\x1D\x1Fend",
			want: "SRC>DST3,qAR," + login + ":lowdata \x1C\x1D\x1Fend"},
		{name: "low removed", send: "SRC>DST3,qAR," + login + ":lowdata end",
			drop: "SRC>DST:low dummy1"},
		{name: "low replaced with spaces", send: "SRC>DST3,qAR," + login + ":lowdata    end",
			drop: "SRC>DST:low dummy2"},

		// DEL characters (0x7f).
		{name: "del original", send: "SRC>DST3,qAR," + login + ":del \x7F\x7Fend",
			want: "SRC>DST3,qAR," + login + ":del \x7F\x7Fend"},
		{name: "del removed", send: "SRC>DST3,qAR," + login + ":del end",
			drop: "SRC>DST:del dummy1"},
		{name: "del replaced with spaces", send: "SRC>DST3,qAR," + login + ":del   end",
			drop: "SRC>DST:del dummy2"},

		// A really long packet, and a duplicate copy of it.
		{name: "long original", send: long, want: long},
		{name: "long dupe", send: long, drop: "SRC>LONG:long dummy"},
	}

	for _, st := range steps {
		t.Run(st.name, func(t *testing.T) {
			if st.drop != "" {
				isclient.ExpectDrop(t, tx, rx, st.send, st.drop)
			} else {
				isclient.TxRx(t, tx, rx, st.send, st.want)
			}
		})
	}

	if err := rx.Close(); err != nil {
		t.Errorf("failed to disconnect rx: %v", err)
	}
	if err := tx.Close(); err != nil {
		t.Errorf("failed to disconnect tx: %v", err)
	}

	s.Stop()
}
