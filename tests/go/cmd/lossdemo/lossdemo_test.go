// A demo peer for viewing the frame loss figures on the status page: it
// peers with a running aprsc as TESTIN-2 over IS2 UDP and streams packets
// with deliberate gaps, reordering and duplicates in the frame numbering.
// Not part of the test suite. Run against a server started with
// cfg-aprsc/is2-basic:
//
//	go test ./cmd/lossdemo -run TestLossDemo -v -timeout 3h
package lossdemo

import (
	"fmt"
	"os"
	"testing"
	"time"

	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

func TestLossDemo(t *testing.T) {
	if os.Getenv("LOSSDEMO") == "" {
		t.Skip("set LOSSDEMO=1 to run the demo peer")
	}
	keys := map[uint8]string{1: "firstkey", 2: "secondkey", 3: "thirdkey"}
	udp := isclient.ListenIS2Peer(t, "127.0.0.1:16405", "TESTIN-2", keys, 2)
	udp.SetDestination(t, "127.0.0.1:16404")
	if err := udp.Negotiate(60 * time.Second); err != nil {
		t.Fatalf("negotiation failed: %v", err)
	}
	t.Log("IS2 peer link up, streaming")

	n := 0
	for {
		n++
		line := fmt.Sprintf("N0CALL-%d>APRS,TCPIP*,qAR,TESTIN-2:>demo packet %d", n%15, n)
		sent := false
		switch {
		case n%97 == 0:
			udp.SetSequence(udp.Sequence() + 1) // one frame lost
		case n%500 == 0:
			udp.SetSequence(udp.Sequence() + 120) // a burst longer than the window
		case n%131 == 0:
			seq := udp.Sequence() // two frames swapped: the next one goes first
			udp.SendMessageSeq(isclient.PacketMessage(line+" (reordered)"), seq+1)
			udp.SetSequence(seq + 2)
			if err := udp.SendMessageSeq(isclient.PacketMessage(line), seq); err != nil {
				t.Fatalf("send failed: %v", err)
			}
			sent = true
		case n%211 == 0:
			udp.SendMessageSeq(isclient.PacketMessage(line+" (dupe)"), udp.Sequence()-1)
		}
		if !sent {
			if err := udp.SendLine(line); err != nil {
				t.Fatalf("send failed: %v", err)
			}
		}
		// drain the socket and answer the pings, so that the server keeps
		// the link on IS2
		if err := udp.AnswerPings(20 * time.Millisecond); err != nil {
			t.Fatalf("read failed: %v", err)
		}
		time.Sleep(180 * time.Millisecond)
	}
}
