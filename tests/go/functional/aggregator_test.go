package functional

import (
	"fmt"
	"math/rand"
	"testing"
	"time"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// Feed data to an aggregator server from two servers and validate that no
// dupes come out. Port of aggregator/10aggr_join.t.
func TestAggregatorJoin(t *testing.T) {
	// the packets are unique to this run, so that a real network is not
	// confused by repeated test runs (the Perl suite passes these in the
	// environment)
	id := fmt.Sprintf("%d_%d", time.Now().Unix(), rand.Intn(1000000))
	const igate = "XX7IG"
	tlmSeq := rand.Intn(1000)

	var packets []string
	for _, src := range []string{"XX7FIT-1", "XX7FIT-2"} {
		packets = append(packets,
			src+">APZMDR,TRACE2-2,qAR,"+igate+":!/0(KkThq\">{2Oaprs.fi system testing COM_"+id,
			src+">APZMDR,TRACE2-2,qAR,"+igate+":>system testing STA_"+id+" status",
			src+">ID,qAR,"+igate+":Stupid testing BEACON_"+id+" beacon",
			src+">APRS,WIDE2-2,qAR,"+igate+"::OH7LZB-10:MSG_"+id,
			fmt.Sprintf("%s>APRS,WIDE2-2,qAR,%s:T#%d,1,010,100,500,900,10101010", src, igate, tlmSeq),
			src+">APRS,WIDE2-2,qAR,"+igate+":_03041723c344s009g014t038r000p000P000h38b10207wDVP",
			// bad packet to force raw packets flush
			src+">APZMDR,TRACE2-2,qAR,"+igate+":!/0(KBAD LAST BAD_"+id,
		)
	}

	fake1 := isclient.ListenFakeUpstream(t, "127.0.0.1:57153", "CORE1")
	fake2 := isclient.ListenFakeUpstream(t, "127.0.0.1:57154", "CORE2")

	s := harness.Start(t, "aggregator")

	up1 := fake1.Accept(t)
	up2 := fake2.Accept(t)

	// create a client connection
	cl := isclient.Connect(t, "localhost:56152", "CL1ENT")

	for _, p := range packets {
		if err := up1.SendLine(p); err != nil {
			t.Fatalf("failed to write packet to socket 1: %v", err)
		}
		if err := up2.SendLine(p); err != nil {
			t.Fatalf("failed to write packet to socket 2: %v", err)
		}
	}

	time.Sleep(3 * time.Second)

	isclient.ExpectSilence(t, up1, time.Second, "read-only upstream connection 1")
	isclient.ExpectSilence(t, up2, 2*time.Second, "read-only upstream connection 2")

	// Read the packets from the client connection, and validate that we get
	// only a single copy of each packet.
	known := map[string]bool{}
	for _, p := range packets {
		known[p] = true
	}
	got := map[string]bool{}
	dupes, unknowns := 0, 0

	for {
		l, err := cl.ReadPacket(time.Second)
		if err != nil {
			break
		}
		switch {
		case !known[l]:
			unknowns++
			t.Logf("unknown packet in the output stream: %q", l)
		case got[l]:
			dupes++
			t.Logf("duplicate packet in the output stream: %q", l)
		default:
			got[l] = true
		}
	}

	if dupes != 0 {
		t.Errorf("found %d duplicate packets in the output stream", dupes)
	}
	if unknowns != 0 {
		t.Errorf("found %d unknown packets in the output stream", unknowns)
	}
	t.Logf("%d of %d distinct packets came out", len(got), len(known))

	s.Stop()
}

// The client and server socket timeouts: an idle uplink is dropped after
// UpstreamTimeout (10 s) and an idle client after ClientTimeout (20 s). Port of
// aggregator/20socket-timeouts.t.
func TestSocketTimeouts(t *testing.T) {
	fake := isclient.ListenFakeUpstream(t, "127.0.0.1:57153", "CORE1")

	s := harness.Start(t, "aggregator")

	up := fake.Accept(t)

	// The server measures the timeout in whole-second ticks from the last
	// read, and the stopwatch starts after the login exchange, so it can
	// legitimately fire up to about a second early.
	start := time.Now()
	if l, err := up.ReadPacket(30 * time.Second); err == nil {
		t.Errorf("got data %q from the server when none should have been available", l)
	}
	if d := time.Since(start); d <= 8*time.Second || d >= 15*time.Second {
		t.Errorf("timeout on the uplink socket took %v, should take 10 s", d)
	}

	// create a client connection
	cl := isclient.Connect(t, "localhost:56152", "CL1ENT")

	start = time.Now()
	if l, err := cl.ReadPacket(60 * time.Second); err == nil {
		t.Errorf("received data %q from the server on the client connection when none should have been available", l)
	}
	if d := time.Since(start); d <= 18*time.Second || d >= 25*time.Second {
		t.Errorf("timeout on the client socket took %v, should take 20 s", d)
	}

	s.Stop()
}
