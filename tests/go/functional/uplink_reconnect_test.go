package functional

import (
	"fmt"
	"strings"
	"testing"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// Uplink reconnection after flooding the upstream output buffer: the fake
// upstream never reads, so the server must give up on the connection and
// connect again. Port of t/51uplink-reconnect.t.
func TestUplinkReconnect(t *testing.T) {
	const rounds = 3

	fake := isclient.ListenFakeUpstream(t, "127.0.0.1:10153", "FAKEUP")

	s := harness.Start(t, "basic")

	tx := isclient.Connect(t, "localhost:55152", "N5CAL-1")

	tail := strings.Repeat(" blaa", 78) + " END\r\n"

	for round := 0; round < rounds; round++ {
		up1 := fake.Accept(t)

		var obuf strings.Builder
		flush := func() {
			if err := tx.SendRaw([]byte(obuf.String())); err != nil {
				t.Fatalf("round %d: failed to send: %v", round, err)
			}
			obuf.Reset()
		}

		for n := 0; n < 10000; n++ {
			fmt.Fprintf(&obuf, "FL%d>APRS,qAR,OH9XYZ-5:!6028.51N/02505.68E# round %d packet %d%s",
				n%10000+10, round, n, tail)
			if obuf.Len() > 32768 {
				flush()
			}
		}
		if obuf.Len() > 0 {
			flush()
		}

		// The server drops the flooded connection and reconnects. The
		// reconnection is closed too, like the flooded one, so that the
		// server connects again right away for the next round, instead of
		// after its upstream timeout.
		up2 := fake.Accept(t)

		up1.Close()
		up2.Close()
	}

	if err := tx.Close(); err != nil {
		t.Errorf("failed to disconnect from the server: %v", err)
	}

	s.Stop()
}
