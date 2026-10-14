package functional

import (
	"encoding/binary"
	"errors"
	"fmt"
	"net/http"
	"strings"
	"testing"
	"time"

	"google.golang.org/protobuf/proto"

	"github.com/hessu/aprsc/tests/go/internal/aprsis2"
	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// IS2 over SCTP: clients on IS2 ports over SCTP, plaintext SCTP clients
// upgrading to IS2 in-band (doc/IS2UPGRADE.md), and an IS2 uplink over SCTP
// between two servers.

// requireIS2SCTP skips the test unless aprsc is built with IS2 and SCTP, and
// this host can make SCTP connections.
func requireIS2SCTP(t *testing.T) {
	t.Helper()

	harness.RequireIS2(t)
	if out, err := harness.CheckConfig(t, "is2-sctp"); err != nil {
		if strings.Contains(out, "Unsupported protocol 'sctp'") {
			t.Skip("this aprsc build has no SCTP support")
		}
		t.Fatalf("configuration check failed: %v\n%s", err, out)
	}
	if err := isclient.SCTPSupported(); err != nil {
		t.Skipf("no SCTP client: %v", err)
	}
}

// expectAll sends a packet from tx, and expects it to come out of each of the
// receivers.
func expectAll(t *testing.T, tx isclient.Sender, packet string, rxs ...isclient.Receiver) {
	t.Helper()

	if err := tx.SendLine(packet); err != nil {
		t.Fatalf("failed to send %q: %v", packet, err)
	}
	for i, rx := range rxs {
		got, err := rx.ReadPacket(isclient.DefaultTimeout)
		if err != nil {
			t.Errorf("receiver %d: no packet: %v (sent %q)", i, err, packet)
			continue
		}
		if got != packet {
			t.Errorf("receiver %d: got %q, want %q", i, got, packet)
		}
	}
}

// statusWithClients fetches the status of the server until it lists at
// least n clients, since the server caches the status for a second or two,
// and returns it with the clients by username.
func statusWithClients(t *testing.T, n int) (*statusJSON, map[string]map[string]any) {
	t.Helper()

	deadline := time.Now().Add(5 * time.Second)
	for {
		j := getStatus(t)
		clients := make(map[string]map[string]any)
		for _, c := range j.Clients {
			name, _ := c["username"].(string)
			clients[name] = c
		}
		if len(clients) >= n || time.Now().After(deadline) {
			return j, clients
		}
		time.Sleep(250 * time.Millisecond)
	}
}

func TestIS2SCTP(t *testing.T) {
	harness.Parallel(t)
	requireIS2SCTP(t)
	s := harness.Start(t, "is2-sctp")

	sctp := isclient.WithSCTP()

	// IS2 ports over SCTP
	is2Full := isclient.ConnectIS2(t, harness.Addr(t, 56152), "N5CAL-1", sctp)
	is2Igate := isclient.ConnectIS2(t, harness.Addr(t, 56580), "N5CAL-2", sctp)

	// a plaintext client sees only the banner before its login reply
	lines := isclient.PreLoginSCTP(t, harness.Addr(t, 55580), upgradeLogin)
	if len(lines) != 2 || !strings.HasPrefix(lines[0], "# aprsc") || !strings.HasPrefix(lines[1], "# logresp") {
		t.Errorf("plaintext SCTP login: got %q, want the banner and a logresp", lines)
	}
	// a hello with no supported version does not upgrade
	checkNoUpgrade(t, "SCTP hello v=2", isclient.PreLoginSCTP(t, harness.Addr(t, 55580), "#is2 hello v=2", upgradeLogin))

	// upgrades on plaintext SCTP ports of each type
	upFull := isclient.ConnectIS2Upgrade(t, harness.Addr(t, 55152), "N5CAL-3", sctp)
	upIgate := isclient.ConnectIS2Upgrade(t, harness.Addr(t, 55580), "N5CAL-4", sctp)
	upClient := isclient.ConnectIS2Upgrade(t, harness.Addr(t, 55581), "N5CAL-5", sctp)
	for _, c := range []*isclient.IS2{upFull, upIgate, upClient} {
		p := c.Preamble
		if len(p) != 2 || !strings.HasPrefix(p[0], "# aprsc") || p[1] != "#is2 hello v=1" {
			t.Errorf("%s: got %q before the switch, want the banner and \"#is2 hello v=1\"", c.Callsign, p)
		}
	}
	for _, c := range []*isclient.IS2{is2Full, is2Igate, upFull, upIgate, upClient} {
		if err := c.Ping(); err != nil {
			t.Errorf("%s: ping failed: %v", c.Callsign, err)
		}
	}

	// not on an SCTP port with noIS2upgrade
	checkNoUpgrade(t, "noIS2upgrade", isclient.PreLoginSCTP(t, harness.Addr(t, 55582), isclient.UpgradeHello, upgradeLogin))
	if _, err := isclient.DialIS2Upgrade(harness.Addr(t, 55582), "N5CAL-9", sctp); !errors.Is(err, isclient.ErrNoUpgrade) {
		t.Errorf("upgrade on a noIS2upgrade port: got error %v, want %v", err, isclient.ErrNoUpgrade)
	}

	plainIgate := isclient.Connect(t, harness.Addr(t, 55580), "N5CAL-6", sctp)
	plainFull := isclient.Connect(t, harness.Addr(t, 55152), "N5CAL-7") // TCP

	// a hello after the login is only a comment
	plainIgate.Send(t, "#is2 hello v=1")

	// packets from each kind of client to the full feeds of each kind
	fulls := []isclient.Receiver{is2Full, upFull, plainFull}
	expectAll(t, plainIgate, "SRC>DST,qAR,IGATE:plaintext SCTP", fulls...)
	expectAll(t, is2Igate, "SRC>DST,qAR,IGATE:IS2 port SCTP", fulls...)
	expectAll(t, upIgate, "SRC>DST,qAR,IGATE:upgraded igate SCTP", fulls...)

	// the status tells which clients are on SCTP and which upgraded, and
	// which listeners offer the upgrade
	type want struct{ link, proto string }
	wantClients := map[string]want{
		"N5CAL-1": {"is2", "sctp"},
		"N5CAL-2": {"is2", "sctp"},
		"N5CAL-3": {"is2", "sctp"},
		"N5CAL-4": {"is2", "sctp"},
		"N5CAL-5": {"is2", "sctp"},
		"N5CAL-6": {"", "sctp"},
		"N5CAL-7": {"", ""},
	}
	j, clients := statusWithClients(t, len(wantClients))
	for name, w := range wantClients {
		c, ok := clients[name]
		if !ok {
			t.Errorf("client %s is not in the status", name)
			continue
		}
		link, _ := c["link"].(string)
		proto, _ := c["proto"].(string)
		if link != w.link || proto != w.proto {
			t.Errorf("status of client %s: link %q proto %q, want link %q proto %q", name, link, proto, w.link, w.proto)
		}
	}
	for _, l := range j.Listeners {
		name, _ := l["name"].(string)
		wantUp := !strings.HasPrefix(name, "IS2 ") && !strings.HasSuffix(name, "no upgrade")
		if up, ok := l["is2_upgrade"].(bool); !ok || up != wantUp {
			t.Errorf("listener %q is2_upgrade is %v, want %v", name, l["is2_upgrade"], wantUp)
		}
	}

	s.Stop()
}

// A load of packets in large IS2 frames, from a client upgraded on a
// plaintext SCTP port to a client on an IS2 SCTP port, which must arrive
// complete and in order.
func TestIS2SCTPLoad(t *testing.T) {
	harness.Parallel(t)
	requireIS2SCTP(t)
	s := harness.Start(t, "is2-sctp")

	const loginTx = "N0GAT"
	rx := isclient.ConnectIS2(t, harness.Addr(t, 56152), "N1GAT", isclient.WithSCTP())
	tx := isclient.ConnectIS2Upgrade(t, harness.Addr(t, 55580), loginTx, isclient.WithSCTP())

	const byteLimit = 4 * 1024 * 1024
	// the bytes in flight; the server disconnects a receiver whose socket
	// send buffer fills up, so this is kept well below its size, counting
	// the kernel's overhead per message
	const window = 32 * 1024
	// frames of about this size, near the largest the server accepts (4096
	// bytes)
	const frameSize = 3500

	var sent []string // not received yet
	outstanding, txn, rxn, txl := 0, 0, 0, 0
	wrong := 0

	read := func(timeout time.Duration) error {
		packets, err := rx.ReadPackets(timeout)
		if err != nil {
			return err
		}
		for _, p := range packets {
			if len(sent) == 0 {
				t.Fatalf("received a packet which was not sent: %q", p)
			}
			if p != sent[0] {
				if wrong++; wrong <= 5 {
					t.Errorf("received %q, want %q", p, sent[0])
				}
			}
			outstanding -= len(sent[0])
			sent = sent[1:]
			rxn++
		}
		return nil
	}

	for txl < byteLimit {
		var batch []string
		n := 0
		for n < frameSize-250 {
			p := fmt.Sprintf("M%d>APRS,qAR,%s:!6028.51N/02505.68E# packet %d %s END",
				txn%10000+10, loginTx, txn, strings.Repeat("A", txn%150))
			batch = append(batch, p)
			n += len(p)
			txn++
		}
		if err := tx.SendPackets(batch...); err != nil {
			t.Fatalf("failed to send: %v", err)
		}
		sent = append(sent, batch...)
		outstanding += n
		txl += n

		for outstanding > window {
			if err := read(isclient.DefaultTimeout); err != nil {
				t.Fatalf("IS2 receiver failed after %d of %d packets: %v", rxn, txn, err)
			}
		}
	}
	for outstanding > 0 {
		if err := read(isclient.DefaultTimeout); err != nil {
			t.Fatalf("IS2 receiver failed after %d of %d packets: %v", rxn, txn, err)
		}
	}

	if wrong > 0 {
		t.Errorf("%d packets out of order or corrupted, of %d", wrong, txn)
	}

	s.Stop()
}

// An IS2 uplink over SCTP: the leaf server connects to the IS2 full feed SCTP
// port of the hub, and packets go through it both ways.
func TestIS2SCTPUplink(t *testing.T) {
	harness.Parallel(t)
	requireIS2SCTP(t)

	hub := harness.Start(t, "is2-sctp")
	leaf := harness.Start(t, "is2-sctp-leaf")

	httpClient := &http.Client{Timeout: 10 * time.Second}
	statusURL := "http://" + harness.AddrOn(t, "127.0.0.1", 55502) + "/status.json"
	var uplink map[string]any
	deadline := time.Now().Add(10 * time.Second)
	for uplink == nil && time.Now().Before(deadline) {
		if uplink = leafUplink(httpClient, statusURL); uplink == nil {
			time.Sleep(200 * time.Millisecond)
		}
	}
	if uplink == nil {
		t.Fatal("the leaf server failed to establish an IS2 SCTP uplink with the hub")
	}
	if uplink["link"] != "is2" || uplink["proto"] != "sctp" {
		t.Errorf("uplink status: link %v proto %v, want is2 sctp", uplink["link"], uplink["proto"])
	}

	hubFull := isclient.Connect(t, harness.Addr(t, 55152), "N1HUB")
	hubIgate := isclient.Connect(t, harness.Addr(t, 55580), "N2HUB", isclient.WithSCTP())
	leafFull := isclient.Connect(t, harness.Addr(t, 55153), "N1LEAF")
	leafIgate := isclient.Connect(t, harness.Addr(t, 55583), "N2LEAF")

	// up from the leaf to the hub, and down from the hub to the leaf
	expectAll(t, leafIgate, "SRC>DST,qAR,N2LEAF:leaf to hub over IS2 SCTP", leafFull, hubFull)
	expectAll(t, hubIgate, "SRC>DST,qAR,N2HUB:hub to leaf over IS2 SCTP", hubFull, leafFull)

	hub.Stop()
	leaf.Stop()
}

// sctpFrame returns the first IS2 message in an SCTP message.
func sctpFrame(t *testing.T, msg []byte) *aprsis2.IS2Message {
	t.Helper()

	if len(msg) < 5 || msg[0] != 0x02 || msg[1] != 0 {
		t.Fatalf("not an IS2 frame: %q", msg)
	}
	length := int(binary.BigEndian.Uint16(msg[2:4]))
	if len(msg) < length+5 || msg[4+length] != 0x03 {
		t.Fatalf("incomplete IS2 frame: %q", msg)
	}
	m := &aprsis2.IS2Message{}
	if err := proto.Unmarshal(msg[4:4+length], m); err != nil {
		t.Fatalf("failed to decode IS2 frame: %v", err)
	}
	return m
}

// The messages of the in-band upgrade and of the IS2 login are sent in
// order, and the ones after the login unordered (doc/SCTP.md).
func TestIS2SCTPDeliveryOrder(t *testing.T) {
	harness.Parallel(t)
	requireIS2SCTP(t)
	harness.Start(t, "is2-sctp")

	rx := dialSCTPMsg(t, harness.Addr(t, 55152))
	if msg := readSCTPMsg(t, rx, "banner", false); !strings.HasPrefix(string(msg), "# aprsc ") {
		t.Errorf("got %q, want the banner", msg)
	}
	if _, err := rx.Write([]byte(isclient.UpgradeHello + "\r\n" + upgradeLogin + "\r\n")); err != nil {
		t.Fatalf("failed to send the hello: %v", err)
	}
	if msg := readSCTPMsg(t, rx, "hello", false); string(msg) != "#is2 hello v=1\r\n" {
		t.Errorf("got %q, want the hello", msg)
	}
	if m := sctpFrame(t, readSCTPMsg(t, rx, "signature", false)); m.GetType() != aprsis2.IS2Message_SERVER_SIGNATURE {
		t.Errorf("got %v, want SERVER_SIGNATURE", m.GetType())
	}

	req, err := proto.Marshal(&aprsis2.IS2Message{
		Type: aprsis2.IS2Message_LOGIN_REQUEST,
		LoginRequest: &aprsis2.IS2LoginRequest{
			Username:   "N0TEST",
			Password:   "-1",
			AppName:    "gotest",
			AppVersion: "1.0",
		},
	})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := rx.Write(isclient.FrameWrap(req)); err != nil {
		t.Fatalf("failed to send the login request: %v", err)
	}
	if m := sctpFrame(t, readSCTPMsg(t, rx, "login reply", false)); m.GetType() != aprsis2.IS2Message_LOGIN_REPLY {
		t.Errorf("got %v, want LOGIN_REPLY", m.GetType())
	}

	tx := isclient.Connect(t, harness.Addr(t, 55580), "N5CAL-6", isclient.WithSCTP())
	tx.Send(t, "SRC>DST,qAR,IGATE:delivery order")
	if m := sctpFrame(t, readSCTPMsg(t, rx, "packet", true)); m.GetType() != aprsis2.IS2Message_IS_PACKET {
		t.Errorf("got %v, want IS_PACKET", m.GetType())
	}
}
