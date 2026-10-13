package functional

import (
	"fmt"
	"net"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// UDP client functions: a client which logs in with a UDP port gets its
// packets over UDP. Port of t/60udp-client.t.
func TestUDPClient(t *testing.T) {
	harness.Parallel(t)
	s := harness.Start(t, "basic")

	const login = "N5CAL-1"
	tx := isclient.Connect(t, harness.Addr(t, 55580), login)
	rx := isclient.ConnectUDP(t, harness.Addr(t, 55152), "N5CAL-2", harness.Port(t, 51342))

	const data = "udp packet content"
	isclient.TxRx(t, tx, rx, "SRC>DST,qAR,"+login+":"+data, "SRC>DST,qAR,"+login+":"+data)

	s.Stop()
}

// UDP core peers. While at it, check that the basic loop prevention rules
// work.
//
//  1. Traffic from upstreams goes to clients.
//  2. Traffic from core peers goes to clients.
//  3. Traffic does not pass between peers and upstreams.
//  4. Traffic from clients goes to core peers and upstreams.
//
// The testing order is selected so that the last packet proves the previous
// ones were not transmitted to the wrong sockets. Port of t/61udp-peer.t.
func TestUDPPeer(t *testing.T) {
	harness.Parallel(t)

	// the peer group in the configuration has the server at 16404 and the
	// peer at 16405
	udp := isclient.ListenUDPPeer(t, harness.AddrOn(t, "127.0.0.1", 16405), harness.AddrOn(t, "127.0.0.1", 16404))
	fake := isclient.ListenFakeUpstream(t, harness.AddrOn(t, "127.0.0.1", 10153), "FAKEUP")

	s := harness.Start(t, "basic")

	const login = "N5CAL-1"
	client := isclient.Connect(t, harness.Addr(t, 55152), login)
	up := fake.Accept(t)

	// 1) from upstream to client
	const fromUpstream = "SRC>DST,qAR,IGATE:upstream to client"
	isclient.TxRx(t, up, client, fromUpstream, fromUpstream)

	// 2) from core peer to client, with trace
	isclient.TxRx(t, udp, client,
		"SRC>DST,qAI,IGATE,SRV1:core peer to client, 1",
		"SRC>DST,qAI,IGATE,SRV1,TESTING:core peer to client, 1")

	// 3) from core peer to client
	const fromPeer = "SRC>DST,qAR,IGATE:core peer to client, 2"
	isclient.TxRx(t, udp, client, fromPeer, fromPeer)

	// 4) from client to peers and upstreams
	const fromClient = "SRC>DST,qAR,IGATE:from client"
	isclient.TxRx(t, client, up, fromClient, fromClient)

	got, err := udp.ReadPacket(isclient.DefaultTimeout)
	if err != nil || got != fromClient {
		t.Errorf("packet from client to UDP peer: got %q (%v), want %q", got, err, fromClient)
	}

	s.Stop()
}

// Without PeerGroupKey lines in the configuration the server has nothing to
// sign IS2 peer frames with, so it must not propose IS2 peering, nor accept it
// when a peer proposes: the link stays on plain UDP.
func TestUDPPeerWithoutKeys(t *testing.T) {
	harness.Parallel(t)
	harness.RequireIS2(t)

	udp := isclient.ListenUDPPeer(t, harness.AddrOn(t, "127.0.0.1", 16405), harness.AddrOn(t, "127.0.0.1", 16404))

	s := harness.Start(t, "basic")

	client := isclient.Connect(t, harness.Addr(t, 55152), "N5CAL-1")

	// plain UDP peering works both ways
	const fromPeer = "SRC>DST,qAR,IGATE:peer to client without keys"
	isclient.TxRx(t, udp, client, fromPeer, fromPeer)
	const fromClient = "SRC>DST,qAR,IGATE:client to peer without keys"
	isclient.TxRx(t, client, udp, fromClient, fromClient)

	// A proposal from the peer (configured as PEER1) is ignored instead of
	// being answered. The server would answer right away if it accepted.
	if err := udp.SendLine("# PEER2 v 2 hello PEER1 vers go-gotest 1.0 token hq736t7q63"); err != nil {
		t.Fatalf("failed to send an IS2 proposal: %v", err)
	}
	isclient.ExpectSilence(t, udp, 3*time.Second, "UDP peer after proposing IS2")
	if n := s.LogCount("control packet ignored, no PeerGroupKey"); n != 1 {
		t.Errorf("the server logged the ignored proposal %d times, want 1", n)
	}

	// and the server never proposed IS2 itself, which it logs when it does
	if n := s.LogCount("proposing IS2"); n != 0 {
		t.Errorf("the server proposed IS2 peering %d times without keys", n)
	}

	// the status tells that the peer link is on plain UDP
	links := getStatus(t).peerLinks()
	if link, ok := links["PEER1"]; !ok || link != "" {
		t.Errorf("status of peer PEER1: link %q (listed %v), want plain UDP (peers: %v)", link, ok, links)
	}

	s.Stop()
}

// HTTP position upload. Port of t/62http-client.t.
func TestHTTPUpload(t *testing.T) {
	harness.Parallel(t)
	s := harness.Start(t, "basic")

	rx := isclient.Connect(t, harness.Addr(t, 55152), "N5CAL-1")

	const data = "TEST>HTAPRS,TCPIP*:>http packet content"
	const out = "TEST>HTAPRS,TCPIP*,qAC,TESTING:>http packet content"
	post := "user TEST pass 29939 vers httpaprstester 1.0\r\n" + data + "\r\n"

	// httpaprstester is the User-Agent of the Perl test, kept as is
	req, err := http.NewRequest("POST", "http://"+harness.AddrOn(t, "127.0.0.1", 55080)+"/", strings.NewReader(post))
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("User-Agent", "httpaprstester/1.0")
	req.Header.Set("Accept-Type", "text/plain")
	req.Header.Set("Content-Type", "application/octet-stream")

	client := &http.Client{
		Timeout: 10 * time.Second,
		CheckRedirect: func(*http.Request, []*http.Request) error {
			return http.ErrUseLastResponse
		},
	}
	res, err := client.Do(req)
	if err != nil {
		t.Fatalf("HTTP POST packet upload failed: %v", err)
	}
	res.Body.Close()
	if res.StatusCode != 200 {
		t.Errorf("HTTP POST packet upload returned %s", res.Status)
	}

	got, err := rx.ReadPacket(isclient.DefaultTimeout)
	if err != nil || got != out {
		t.Errorf("wrong line through HTTP uploading: got %q (%v), want %q", got, err, out)
	}

	s.Stop()
}

// UDP submit: packets sent in a datagram to the UDP submit port are accepted
// with a valid login and rejected without. Port of t/63udp-submit.t.
func TestUDPSubmit(t *testing.T) {
	harness.Parallel(t)
	s := harness.Start(t, "basic")

	rx := isclient.Connect(t, harness.Addr(t, 55152), "OH0CAL-2")

	usock, err := net.Dial("udp", harness.AddrOn(t, "127.0.0.1", 55080))
	if err != nil {
		t.Fatalf("failed to set up an UDP client socket: %v", err)
	}
	defer usock.Close()

	send := func(datagram string) {
		t.Helper()
		if n, err := usock.Write([]byte(datagram)); err != nil || n != len(datagram) {
			t.Fatalf("failed to send a %d byte datagram: %d, %v", len(datagram), n, err)
		}
	}

	const header = "user TEST pass 29939 vers udpaprstester 1.0\r\n"

	send(header + "TEST>UDAPRS,TCPIP*:>udp packet content")
	got, err := rx.ReadPacket(isclient.DefaultTimeout)
	const want = "TEST>UDAPRS,TCPIP*,qAU,TESTING:>udp packet content"
	if err != nil || got != want {
		t.Errorf("wrong line when sent using UDP: got %q (%v), want %q", got, err, want)
	}

	// test rejections
	for _, rej := range []struct{ src, passcode, data string }{
		{"UDROP", "", "UDROP>UDAPRS:>udp packet rejected, no passcode"},
		{"UDROP", "123", "UDROP>UDAPRS:>udp packet rejected, bad passcode"},
		{"UDROPASDSJSKD", "13293", "UDROPASDSJSKD>UDAPRS:>udp packet rejected, too long login callsign"},
		{"UDRO/OH", "29929", "UDROP>UDAPRS:>udp packet rejected, invalid login callsign"},
	} {
		send(fmt.Sprintf("user %s pass %s vers udpaprstester 1.0\r\n%s", rej.src, rej.passcode, rej.data))
	}

	// An oversized datagram, which does not fit in the server's receive
	// buffer: with MSG_TRUNC, Linux reports the full length of a truncated
	// datagram, and it must not be processed.
	send(header + strings.Repeat("x", 2500))

	// another success after the failures
	send(header + "TEST>UDAPRS:>udp packet content 2\r\n")
	got, err = rx.ReadPacket(isclient.DefaultTimeout)
	const want2 = "TEST>UDAPRS,TCPIP*,qAU,TESTING:>udp packet content 2"
	if err != nil || got != want2 {
		t.Errorf("wrong line when sent using UDP after rejections: got %q (%v), want %q", got, err, want2)
	}

	s.Stop()
}

// unorderedLoad sends packets with send until byteLimit bytes have been sent
// and expects every one of them to come out of rx, in any order, with at most
// window bytes outstanding.
func unorderedLoad(t *testing.T, prefix string, send func(line string) error, rx isclient.Receiver) {
	t.Helper()

	const byteLimit = 64 * 1024
	// Bytes in flight. Linux counts the whole buffer of each small datagram
	// against the socket receive buffer, so the server's 64 KiB receive buffer
	// holds only about 170 of these ~80 byte packets. 12 KiB, about 150
	// packets, overflows it when the server thread is descheduled on a busy CI
	// runner.
	const window = 6 * 1024

	expected := map[string]bool{}
	outstanding, txn, rxn, txl, rxl := 0, 0, 0, 0, 0

	received := func(line string) {
		t.Helper()
		if !expected[line] {
			t.Fatalf("received wrong packet: %q", line)
		}
		delete(expected, line)
		outstanding -= len(line) + 2
		rxn++
		rxl += len(line) + 2
	}

	start := time.Now()

	for txl < byteLimit {
		line := fmt.Sprintf("%s%d>APRS,qAR,OH9XYZ-5:!6028.51N/02505.68E# packet %d blaa blaa blaa blaa END",
			prefix, txn%10000+10, txn)
		expected[line] = true
		if err := send(line); err != nil {
			t.Fatalf("failed to send: %v", err)
		}
		txl += len(line) + 2
		outstanding += len(line) + 2
		txn++

		for outstanding > window {
			got, err := rx.ReadPacket(time.Second)
			if err != nil {
				break
			}
			received(got)
		}
	}

	t.Logf("reading the rest, have received %d packets, sent %d", rxn, txn)
	for outstanding > 0 {
		got, err := rx.ReadPacket(time.Second)
		if err != nil {
			t.Logf("read ended: %v", err)
			break
		}
		received(got)
	}
	t.Logf("after reading the rest, have received %d packets, sent %d, outstanding %d bytes", rxn, txn, outstanding)

	dur := time.Since(start)
	t.Logf("took %.3f seconds, %.0f packets/sec", dur.Seconds(), float64(rxn)/dur.Seconds())

	if outstanding != 0 {
		var missing []string
		for l := range expected {
			missing = append(missing, l)
		}
		t.Logf("%d packets missing", len(missing))
	}

	if rxn != txn {
		t.Errorf("received %d lines, sent %d", rxn, txn)
	}
	if rxl != txl {
		t.Errorf("received %d bytes, sent %d", rxl, txl)
	}
	if outstanding != 0 {
		t.Errorf("%d bytes outstanding in the server after timeout", outstanding)
	}
}

// UDP core peers with a load chunk, in both directions. It is skipped with
// -short. Port of t/64udp-load.t.
func TestUDPLoad(t *testing.T) {
	harness.Parallel(t)
	if testing.Short() {
		t.Skip("skipping the UDP load test in short mode")
	}

	udp := isclient.ListenUDPPeer(t, harness.AddrOn(t, "127.0.0.1", 16405), harness.AddrOn(t, "127.0.0.1", 16404))

	s := harness.Start(t, "basic")

	full := isclient.Connect(t, harness.Addr(t, 55152), "N5CAL-1")

	t.Run("full feed to UDP peer", func(t *testing.T) {
		unorderedLoad(t, "F", full.SendLine, udp)
	})

	t.Run("UDP peer to full feed", func(t *testing.T) {
		unorderedLoad(t, "U", func(line string) error {
			return udp.SendRaw([]byte(line + "\r\n"))
		}, full)
	})

	if err := full.Close(); err != nil {
		t.Errorf("failed to disconnect from the server: %v", err)
	}
	time.Sleep(100 * time.Millisecond) // let the server catch and log the disconnect

	s.Stop()
}
