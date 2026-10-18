package functional

import (
	"bufio"
	"net"
	"strings"
	"testing"
	"time"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// tryConnect connects to addr and reports whether the server let the client
// in: an accepted client gets the server banner, a denied one gets a
// "# Server full" or "# Port full" line, or just a close.
func tryConnect(t *testing.T, addr string) bool {
	t.Helper()

	conn, err := net.DialTimeout("tcp", addr, 3*time.Second)
	if err != nil {
		t.Fatalf("failed to connect to %s: %v", addr, err)
	}
	t.Cleanup(func() { conn.Close() })

	conn.SetReadDeadline(time.Now().Add(3 * time.Second))
	line, err := bufio.NewReader(conn).ReadString('\n')
	if err != nil {
		t.Logf("%s: denied, connection closed without a banner: %v", addr, err)
		return false
	}
	line = strings.TrimRight(line, "\r\n")
	if strings.HasPrefix(line, "# aprsc ") {
		return true
	}
	t.Logf("%s: denied: %s", addr, line)
	return false
}

// TestUplinkMaxClients checks that MaxClients counts inbound clients only,
// and that the count stays right over uplink reconnections. The uplink
// closing used to decrement the inbound client count it was never added
// to, which raised the effective MaxClients limit by one per uplink
// reconnection.
func TestUplinkMaxClients(t *testing.T) {
	harness.Parallel(t)

	fake := isclient.ListenFakeUpstream(t, harness.AddrOn(t, "127.0.0.1", 10153), "FAKEUP")
	harness.Start(t, "maxclients")
	up := fake.Accept(t)

	full := harness.Addr(t, 55152)
	igate := harness.Addr(t, 55580)

	// an uplink connection does not count: both clients fit in MaxClients 2
	c1 := isclient.Connect(t, full, "N5CAL-1")
	c2 := isclient.Connect(t, full, "N5CAL-2")

	if tryConnect(t, igate) {
		t.Fatal("a third client was accepted over MaxClients 2")
	}

	// drop the uplink and let the server reconnect, twice
	for i := 0; i < 2; i++ {
		up.Close()
		up = fake.Accept(t)
	}

	if tryConnect(t, igate) {
		t.Fatal("a third client was accepted over MaxClients 2 after uplink reconnections")
	}

	// the limit is not higher than before the reconnections either: two
	// clients fit again once the first two are gone
	c1.Close()
	c2.Close()
	waitForClose := func(addr string) {
		deadline := time.Now().Add(5 * time.Second)
		for !tryConnect(t, addr) {
			if time.Now().After(deadline) {
				t.Fatalf("no client accepted on %s after the earlier ones closed", addr)
			}
			time.Sleep(50 * time.Millisecond)
		}
	}
	waitForClose(igate)
	if !tryConnect(t, full) {
		t.Fatal("a second client was denied under MaxClients 2 after the earlier ones closed")
	}
	if tryConnect(t, full) {
		t.Fatal("a third client was accepted over MaxClients 2 after the earlier ones closed")
	}
}
