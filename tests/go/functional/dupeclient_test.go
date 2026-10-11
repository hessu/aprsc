package functional

import (
	"testing"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// A client on the dupefeed port receives the duplicates the full feed does not
// get. Port of the plaintext part of t/65dupeclient.t.
func TestDupeClient(t *testing.T) {
	s := harness.Start(t, "basic")

	const login = "N5CAL-1"
	tx := isclient.Connect(t, "localhost:55580", login)
	rx := isclient.Connect(t, "localhost:55152", "N5CAL-2")
	dup := isclient.Connect(t, "localhost:55153", "DUPS")

	// A unique packet comes out on the full feed.
	l := "SRC>DST,qAR," + login + ":foo"
	isclient.TxRx(t, tx, rx, l, l)

	// Sent again, it only comes out on the dupe socket.
	isclient.ExpectDropExact(t, tx, rx, l, "SRC>DST:dummy")

	got, err := dup.ReadPacket(isclient.DefaultTimeout)
	if err != nil {
		t.Fatalf("nothing on the dupe socket: %v", err)
	}
	if want := "dup\t" + l; got != want {
		t.Errorf("wrong line on the dupe socket: got %q, want %q", got, want)
	}

	s.Stop()
}

// The same with an IS2 client on the IS2 dupefeed port, which gets the packet
// marked as a duplicate packet type, without the "dup" prefix. Port of the IS2
// part of t/65dupeclient.t.
func TestDupeClientIS2(t *testing.T) {
	harness.RequireIS2(t)
	s := harness.Start(t, "is2-basic")

	const login = "N5CAL-1"
	tx := isclient.Connect(t, "localhost:55580", login)
	rx := isclient.Connect(t, "localhost:55152", "N5CAL-2")
	dup := isclient.Connect(t, "localhost:55153", "DUPS")
	dupIS2 := isclient.ConnectIS2(t, "localhost:56153", "DUPIS2", isclient.WithDuplicates())

	l := "SRC>DST,qAR," + login + ":foo"
	isclient.TxRx(t, tx, rx, l, l)
	isclient.ExpectDropExact(t, tx, rx, l, "SRC>DST:dummy")

	got, err := dup.ReadPacket(isclient.DefaultTimeout)
	if err != nil || got != "dup\t"+l {
		t.Errorf("wrong line on the dupe socket: got %q (%v), want %q", got, err, "dup\t"+l)
	}

	got, err = dupIS2.ReadPacket(isclient.DefaultTimeout)
	if err != nil || got != l {
		t.Errorf("wrong line on the IS2 dupe socket: got %q (%v), want %q", got, err, l)
	}

	s.Stop()
}
