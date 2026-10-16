package functional

import (
	"encoding/json"
	"io"
	"net/http"
	"slices"
	"sort"
	"strings"
	"testing"
	"time"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// SCTP, only on aprsc: two servers, where the leaf connects to the hub with an
// SCTP uplink, and a few megabytes of traffic go through it. It is skipped if
// this build of aprsc has no SCTP support (the configuration is rejected).
// Port of t/69sctp.t.
func TestSCTP(t *testing.T) {
	harness.Parallel(t)
	requireSCTP(t)

	hub := harness.Start(t, "sctp-hub")
	leaf := harness.Start(t, "sctp-leaf")

	// wait for the leaf to connect to the hub
	httpClient := &http.Client{Timeout: 10 * time.Second}
	statusURL := "http://" + harness.AddrOn(t, "127.0.0.1", 55502) + "/status.json"
	var uplink map[string]any

	deadline := time.Now().Add(10 * time.Second)
	for uplink == nil && time.Now().Before(deadline) {
		uplink = leafUplink(httpClient, statusURL)
		if uplink == nil {
			time.Sleep(time.Second)
		}
	}
	if uplink == nil {
		t.Fatal("leaf server failed to establish an SCTP uplink with the hub")
	}

	const loginTx = "N0GAT"
	rx := isclient.Connect(t, harness.Addr(t, 55152), "N1GAT")
	tx := isclient.Connect(t, harness.Addr(t, 55583), loginTx)

	orderedLoad(t, tx, rx, loginTx, 4*1024*1024, 2*time.Second)

	hub.Stop()
	leaf.Stop()
}

// requireSCTP skips the test unless aprsc is built with SCTP support (the
// configuration is rejected otherwise).
func requireSCTP(t *testing.T) {
	t.Helper()

	harness.RequireAprsc(t)
	if out, err := harness.CheckConfig(t, "sctp-hub"); err != nil {
		if strings.Contains(out, "Unsupported protocol 'sctp'") {
			t.Skip("this aprsc build has no SCTP support")
		}
		t.Fatalf("configuration check failed: %v\n%s", err, out)
	}
}

// dialSCTPMsg connects to addr over SCTP for reading whole messages. The
// connection is closed when the test finishes.
func dialSCTPMsg(t *testing.T, addr string) *isclient.SCTPMsgConn {
	t.Helper()

	c, err := isclient.DialSCTPMsg(addr, isclient.DefaultTimeout)
	if err != nil {
		t.Fatalf("SCTP connect to %s failed: %v", addr, err)
	}
	t.Cleanup(func() { c.Close() })
	return c
}

// readSCTPMsg reads a message from c, and checks whether it was sent
// unordered.
func readSCTPMsg(t *testing.T, c *isclient.SCTPMsgConn, what string, unordered bool) []byte {
	t.Helper()

	msg, u, err := c.ReadMsg(isclient.DefaultTimeout)
	if err != nil {
		t.Fatalf("%s: no message received: %v", what, err)
	}
	if u != unordered {
		t.Errorf("%s: sent unordered %v, want %v: %q", what, u, unordered, msg)
	}
	return msg
}

// The server sends the messages of the login handshake in order, so that a
// retransmission can not reorder them, and the ones after the login
// unordered (doc/SCTP.md).
func TestSCTPDeliveryOrder(t *testing.T) {
	harness.Parallel(t)
	requireSCTP(t)
	if err := isclient.SCTPSupported(); err != nil {
		t.Skipf("no SCTP client: %v", err)
	}

	harness.Start(t, "sctp-hub")

	rx := dialSCTPMsg(t, harness.Addr(t, 55152))
	if msg := readSCTPMsg(t, rx, "banner", false); !strings.HasPrefix(string(msg), "# aprsc ") {
		t.Errorf("got %q, want the banner", msg)
	}
	if _, err := rx.Write([]byte("user N1GAT pass -1 vers gotest 1.0\r\n")); err != nil {
		t.Fatalf("failed to send login: %v", err)
	}
	if msg := readSCTPMsg(t, rx, "logresp", false); !strings.HasPrefix(string(msg), "# logresp N1GAT ") {
		t.Errorf("got %q, want the logresp", msg)
	}

	tx := isclient.Connect(t, harness.Addr(t, 55582), "N0GAT")
	tx.Send(t, "N0GAT>APRS:>delivery order")
	if msg := readSCTPMsg(t, rx, "packet", true); !strings.HasPrefix(string(msg), "N0GAT>APRS,") {
		t.Errorf("got %q, want the packet", msg)
	}
}

// The server starts sending packets to a client after the login only when
// the client has acknowledged the login handshake, so that the unordered
// packets can not overtake it. A client which delays its acknowledgements
// does not get a packet sent before then.
func TestSCTPDataAfterLoginAck(t *testing.T) {
	harness.Parallel(t)
	requireSCTP(t)
	if err := isclient.SCTPSupported(); err != nil {
		t.Skipf("no SCTP client: %v", err)
	}

	harness.Start(t, "sctp-hub")

	tx := isclient.Connect(t, harness.Addr(t, 55582), "N0GAT")

	const ackDelay = 500 * time.Millisecond
	rx := dialSCTPMsg(t, harness.Addr(t, 55152))
	if err := rx.SetAckDelay(ackDelay); err != nil {
		t.Fatal(err)
	}
	readSCTPMsg(t, rx, "banner", false)
	if _, err := rx.Write([]byte("user N1GAT pass -1 vers gotest 1.0\r\n")); err != nil {
		t.Fatalf("failed to send login: %v", err)
	}
	readSCTPMsg(t, rx, "logresp", false)

	// before the logresp is acknowledged
	tx.Send(t, "N0GAT>APRS:>before ack")
	// the time it takes is what it is about
	time.Sleep(2 * ackDelay)
	tx.Send(t, "N0GAT>APRS:>after ack")

	msg := readSCTPMsg(t, rx, "packet", true)
	if !strings.HasPrefix(string(msg), "N0GAT>APRS,") || !strings.HasSuffix(string(msg), ":>after ack\r\n") {
		t.Errorf("got %q, want the packet sent after the acknowledgement", msg)
	}
}

// leafUplink returns the first uplink in the leaf's status.json at url, if
// it has logged in to the hub (as TESTING-1).
func leafUplink(c *http.Client, url string) map[string]any {
	res, err := c.Get(url)
	if err != nil {
		return nil
	}
	defer res.Body.Close()
	if res.StatusCode != 200 {
		return nil
	}

	body, err := io.ReadAll(res.Body)
	if err != nil {
		return nil
	}

	var j struct {
		Uplinks []map[string]any `json:"uplinks"`
	}
	if json.Unmarshal(body, &j) != nil || len(j.Uplinks) == 0 {
		return nil
	}

	if j.Uplinks[0]["username"] == "TESTING-1" {
		return j.Uplinks[0]
	}
	return nil
}

// An SCTP uplink with both an IPv4 and an IPv6 UplinkBind address is
// multi-homed on the two: the upstream sees both addresses of the server,
// although the server connected to its IPv4 address, and packets go through.
func TestSCTPUplinkMultihoming(t *testing.T) {
	harness.Parallel(t)
	requireSCTP(t)
	if err := isclient.SCTPSupported(); err != nil {
		t.Skipf("no SCTP client: %v", err)
	}

	const upstreamCall = "FAKEUP"
	fake := isclient.ListenFakeUpstreamSCTP(t, harness.AddrOn(t, "::", 54153), upstreamCall)

	s := harness.Start(t, "sctp-multihome")
	up := fake.Accept(t)

	addrs, err := up.PeerAddrs()
	if err != nil {
		t.Fatal(err)
	}
	sort.Strings(addrs)
	if want := []string{"127.0.0.1", "::1"}; !slices.Equal(addrs, want) {
		t.Errorf("the uplink association has the addresses %v, want %v", addrs, want)
	}

	rx := isclient.Connect(t, harness.Addr(t, 55152), "N1GAT")
	isclient.RunSteps(t, up, rx, []isclient.Step{
		passSame("packet from the uplink", "N0GAT>APRS,qAR,"+upstreamCall+":>multi-homed"),
	})

	s.Stop()
}
