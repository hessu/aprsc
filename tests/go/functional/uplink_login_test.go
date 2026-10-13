package functional

import (
	"testing"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// The upstream server may send other comment lines before its logresp: a
// line after the banner announcing a capability, or its keepalive beacon if
// the login takes a while. The uplink must skip them and wait for the
// logresp, instead of closing the connection as a protocol error.
func TestUplinkLoginComments(t *testing.T) {
	harness.Parallel(t)
	harness.RequireAprsc(t)

	fake := isclient.ListenFakeUpstream(t, harness.AddrOn(t, "127.0.0.1", 10153), "FAKEUP")
	fake.AfterBanner = []string{"#is2 v=1", "#  a comment with more spaces", "#logresp-like but not one"}
	fake.BeforeLogresp = []string{"# IS_Fake 1.00 14 Oct 2026 00:00:00 GMT FAKEUP 127.0.0.1:10153"}

	s := harness.Start(t, "basic")
	up := fake.Accept(t)

	// logged in: a packet from a client goes up
	tx := isclient.Connect(t, harness.Addr(t, 55152), "N5CAL-1")
	const pkt = "SRC>DST,qAR,IGATE:uplink login with comments before the logresp"
	tx.Send(t, pkt)
	if got, err := up.ReadPacket(isclient.DefaultTimeout); err != nil || got != pkt {
		t.Errorf("fake upstream: got %q (%v), want %q", got, err, pkt)
	}

	if n := s.LogCount("Uplink server comment before login response, skipped"); n != 4 {
		t.Errorf("the server skipped %d comment lines before the logresp, want 4", n)
	}
	if n := s.LogCount("Uplink logged in to server FAKEUP"); n != 1 {
		t.Errorf("the server logged in to the upstream %d times, want 1", n)
	}

	s.Stop()
}
