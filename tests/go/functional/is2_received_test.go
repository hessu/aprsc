package functional

import (
	"bytes"
	"fmt"
	"testing"
	"time"

	"github.com/hessu/aprsc/tests/go/internal/aprsis2"
	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// The Received entries of IS2 packets (doc/IS2RECEIVED.md): the server
// appends an entry naming itself, the client the packet came from and the
// client's address, keeps the newest five entries, stamps the packets with
// received_time, and can drop packets by that stamp.

// received builds a Received entry.
func received(server, user string, v aprsis2.VerificationStatus, addr []byte) *aprsis2.IS2Received {
	return &aprsis2.IS2Received{ServerId: server, Username: user, Verified: v, ClientAddr: addr}
}

// loopback is the address the test clients connect from, as the entries
// carry it.
var loopback = []byte{127, 0, 0, 1}

// The packets are sent from clients on the igate ports, with the login in
// the path, which the server replaces with a qAR construct.
const receivedPath = "SRC>APRS,OH2RDG*,WIDE,"

func sentPacket(login, text string) string { return receivedPath + login + ",I:" + text }
func passedPacket(login, text string) string {
	return receivedPath + "qAR," + login + ":" + text
}

// receivedString formats a list of entries for a test failure message.
func receivedString(rs []*aprsis2.IS2Received) string {
	var b bytes.Buffer
	for _, r := range rs {
		fmt.Fprintf(&b, " [%s from %s %v %v]", r.GetServerId(), r.GetUsername(), r.GetVerified(), r.GetClientAddr())
	}
	return b.String()
}

// checkReceived checks the Received entries of a packet.
func checkReceived(t *testing.T, what string, got *aprsis2.ISPacket, want []*aprsis2.IS2Received) {
	t.Helper()

	rs := got.GetReceived()
	same := len(rs) == len(want)
	for i := 0; same && i < len(rs); i++ {
		same = rs[i].GetServerId() == want[i].GetServerId() &&
			rs[i].GetUsername() == want[i].GetUsername() &&
			rs[i].GetVerified() == want[i].GetVerified() &&
			bytes.Equal(rs[i].GetClientAddr(), want[i].GetClientAddr())
	}
	if !same {
		t.Errorf("%s: Received entries of %q differ:\n got %s\n want%s",
			what, got.GetIsPacketData(), receivedString(rs), receivedString(want))
	}
}

// readISPacket reads the next packet from an IS2 client or UDP peer, and
// checks that it is the one expected.
func readISPacket(t *testing.T, rx interface {
	ReadISPacket(timeout time.Duration) (*aprsis2.ISPacket, error)
}, want string) *aprsis2.ISPacket {
	t.Helper()

	got, err := rx.ReadISPacket(isclient.DefaultTimeout)
	if err != nil {
		t.Fatalf("did not receive %q: %v", want, err)
	}
	if string(got.GetIsPacketData()) != want {
		t.Fatalf("received %q, want %q", got.GetIsPacketData(), want)
	}
	return got
}

// checkStamp checks that the received_time of a packet is close to now.
func checkStamp(t *testing.T, what string, got *aprsis2.ISPacket) {
	t.Helper()

	age := time.Now().UnixMilli() - int64(got.GetReceivedTime())
	if age < -1000 || age > 5000 {
		t.Errorf("%s: received_time %d is %d ms from now", what, got.GetReceivedTime(), age)
	}
}

// total returns a counter of the totals in the status. The status is cached
// by the server for a couple of seconds, so the value may be stale.
func total(t *testing.T, name string) int {
	t.Helper()

	v, ok := getStatus(t).Totals[name].(float64)
	if !ok {
		t.Fatalf("status totals have no %s", name)
	}
	return int(v)
}

// expectTotal waits for a counter of the totals to reach the value wanted.
func expectTotal(t *testing.T, name string, want int) {
	t.Helper()

	deadline := time.Now().Add(5 * time.Second)
	for {
		got := total(t, name)
		if got == want {
			return
		}
		if time.Now().After(deadline) {
			t.Errorf("%s is %d, want %d", name, got, want)
			return
		}
		time.Sleep(200 * time.Millisecond)
	}
}

func TestIS2Received(t *testing.T) {
	harness.Parallel(t)
	harness.RequireIS2(t)

	// an UDP peer, which gets the addresses in the entries like any other
	// server
	udp := isclient.ListenIS2Peer(t, harness.AddrOn(t, "127.0.0.1", 16405), "TESTIN-2", is2PeerKeys, 2)
	udp.SetDestination(t, harness.AddrOn(t, "127.0.0.1", 16404))
	s := harness.Start(t, "is2-basic")
	if err := udp.Negotiate(20 * time.Second); err != nil {
		t.Fatalf("failed to negotiate the IS2 corepeer link: %v", err)
	}

	const plain = "N0PLAIN"
	const client = "N0IS2"
	rx := isclient.ConnectIS2(t, harness.Addr(t, 56152), "N1RX")
	txPlain := isclient.Connect(t, harness.Addr(t, 55580), plain)
	tx := isclient.ConnectIS2(t, harness.Addr(t, 56580), client)

	ours := received("TESTING", client, aprsis2.VerificationStatus_WEAK, nil)
	oursAddr := received("TESTING", client, aprsis2.VerificationStatus_WEAK, loopback)

	// send sends a packet with the given entries and stamp from the IS2
	// client, and returns the packet as it should come out.
	send := func(t *testing.T, text string, stamp uint64, rs ...*aprsis2.IS2Received) string {
		t.Helper()
		p := isclient.NewISPacket(sentPacket(client, text), isclient.Meta{})
		p.Received = rs
		p.ReceivedTime = stamp
		if err := tx.SendISPacket(p); err != nil {
			t.Fatalf("failed to send %q: %v", text, err)
		}
		return passedPacket(client, text)
	}

	// A packet from a plaintext client gets one entry, without the address
	// for the IS2 client, and with it for the peer, and is stamped.
	t.Run("plaintext client", func(t *testing.T) {
		txPlain.Send(t, sentPacket(plain, "from a plaintext client"))
		packet := passedPacket(plain, "from a plaintext client")

		got := readISPacket(t, rx, packet)
		checkReceived(t, "client", got, []*aprsis2.IS2Received{received("TESTING", plain, aprsis2.VerificationStatus_WEAK, nil)})
		checkStamp(t, "client", got)

		got = readISPacket(t, udp, packet)
		checkReceived(t, "peer", got, []*aprsis2.IS2Received{received("TESTING", plain, aprsis2.VerificationStatus_WEAK, loopback)})
		checkStamp(t, "peer", got)
	})

	// The entries a packet comes with are kept, ours is added last, and
	// the stamp is kept. The addresses go to the peer only.
	t.Run("entries and stamp kept", func(t *testing.T) {
		stamp := uint64(time.Now().UnixMilli() - 1500)
		packet := send(t, "with entries", stamp,
			received("S1", "U1", aprsis2.VerificationStatus_STRONG, []byte{10, 0, 0, 1}),
			received("S2", "S1", aprsis2.VerificationStatus_WEAK, nil))

		got := readISPacket(t, rx, packet)
		checkReceived(t, "client", got, []*aprsis2.IS2Received{
			received("S1", "U1", aprsis2.VerificationStatus_STRONG, nil),
			received("S2", "S1", aprsis2.VerificationStatus_WEAK, nil),
			ours,
		})
		if got.GetReceivedTime() != stamp {
			t.Errorf("client: received_time %d, want %d", got.GetReceivedTime(), stamp)
		}

		got = readISPacket(t, udp, packet)
		checkReceived(t, "peer", got, []*aprsis2.IS2Received{
			received("S1", "U1", aprsis2.VerificationStatus_STRONG, []byte{10, 0, 0, 1}),
			received("S2", "S1", aprsis2.VerificationStatus_WEAK, nil),
			oursAddr,
		})
		if got.GetReceivedTime() != stamp {
			t.Errorf("peer: received_time %d, want %d", got.GetReceivedTime(), stamp)
		}
	})

	// Only the newest five entries are kept, ours among them.
	t.Run("truncated to five", func(t *testing.T) {
		var six []*aprsis2.IS2Received
		for i := 1; i <= 6; i++ {
			six = append(six, received(fmt.Sprintf("S%d", i), fmt.Sprintf("U%d", i), aprsis2.VerificationStatus_WEAK, nil))
		}
		packet := send(t, "with six entries", 0, six...)

		got := readISPacket(t, rx, packet)
		checkReceived(t, "client", got, append(six[2:], ours))
		readISPacket(t, udp, packet)

		// the sixth entry in, and the oldest when ours was added
		expectTotal(t, "is2_received_truncated", 2)
	})

	// Malformed entries are dropped, the packet and the rest of the
	// entries are kept.
	t.Run("invalid entries dropped", func(t *testing.T) {
		v6 := []byte{0xfd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}
		packet := send(t, "with invalid entries", 0,
			received("", "U1", aprsis2.VerificationStatus_WEAK, nil),
			received("S2", "", aprsis2.VerificationStatus_WEAK, nil),
			received("S3", "U3", aprsis2.VerificationStatus(9), nil),
			received("S4", "U4", aprsis2.VerificationStatus_WEAK, []byte{1, 2, 3, 4, 5}),
			received("S5", "U 5", aprsis2.VerificationStatus_WEAK, nil),
			received("S6TOOLONGSERVERID", "U6", aprsis2.VerificationStatus_WEAK, nil),
			received("S7", "U7", aprsis2.VerificationStatus_MEDIUM, v6),
		)

		got := readISPacket(t, rx, packet)
		checkReceived(t, "client", got, []*aprsis2.IS2Received{
			received("S7", "U7", aprsis2.VerificationStatus_MEDIUM, nil),
			ours,
		})
		got = readISPacket(t, udp, packet)
		checkReceived(t, "peer", got, []*aprsis2.IS2Received{
			received("S7", "U7", aprsis2.VerificationStatus_MEDIUM, v6),
			oursAddr,
		})

		expectTotal(t, "is2_received_invalid", 6)
	})

	// A packet which already has an entry of this server has looped: it
	// is counted, and passed on.
	t.Run("loop counted", func(t *testing.T) {
		packet := send(t, "been here before", 0, received("TESTING", "N0OTHER", aprsis2.VerificationStatus_WEAK, nil))

		got := readISPacket(t, rx, packet)
		checkReceived(t, "client", got, []*aprsis2.IS2Received{
			received("TESTING", "N0OTHER", aprsis2.VerificationStatus_WEAK, nil),
			ours,
		})
		readISPacket(t, udp, packet)

		expectTotal(t, "is2_received_loops", 1)
	})

	// A packet from a peer gets an entry naming the peer.
	t.Run("from a peer", func(t *testing.T) {
		const packet = "SRC>DST,qAR,IGATE:from a peer"
		p := isclient.NewISPacket(packet, isclient.Meta{})
		p.Received = []*aprsis2.IS2Received{received("TESTIN-2", "N0FAR", aprsis2.VerificationStatus_STRONG, []byte{10, 1, 1, 1})}
		if err := udp.SendMessage(&aprsis2.IS2Message{
			Type:     aprsis2.IS2Message_IS_PACKET,
			IsPacket: []*aprsis2.ISPacket{p},
		}); err != nil {
			t.Fatalf("failed to send from the peer: %v", err)
		}

		got := readISPacket(t, rx, packet)
		checkReceived(t, "client", got, []*aprsis2.IS2Received{
			received("TESTIN-2", "N0FAR", aprsis2.VerificationStatus_STRONG, nil),
			received("TESTING", "TESTIN-2", aprsis2.VerificationStatus_WEAK, nil),
		})
		checkStamp(t, "client", got)
	})

	// Packets too old or too new by their stamp are counted, but passed on
	// unless IS2ReceivedAgeDrop is enabled.
	t.Run("age counted", func(t *testing.T) {
		packet := send(t, "from the past", uint64(time.Now().Add(-60*time.Second).UnixMilli()))
		readISPacket(t, rx, packet)
		readISPacket(t, udp, packet)

		packet = send(t, "from the future", uint64(time.Now().Add(60*time.Second).UnixMilli()))
		readISPacket(t, rx, packet)
		readISPacket(t, udp, packet)

		packet = send(t, "from a moment ago", uint64(time.Now().Add(-10*time.Second).UnixMilli()))
		readISPacket(t, rx, packet)
		readISPacket(t, udp, packet)

		expectTotal(t, "is2_received_too_old", 1)
		expectTotal(t, "is2_received_too_new", 1)
		s.WaitLog("seconds in the past", 1, 5*time.Second)
	})

	s.Stop()
}

// With IS2ReceivedAgeDrop, the packets too far from our clock are dropped.
func TestIS2ReceivedAgeDrop(t *testing.T) {
	harness.Parallel(t)
	harness.RequireIS2(t)
	s := harness.StartText(t, "is2-received-agedrop", is2BasicWith(t, nil, "IS2ReceivedAgeDrop true"))

	const client = "N0IS2"
	rx := isclient.ConnectIS2(t, harness.Addr(t, 56152), "N1RX")
	tx := isclient.ConnectIS2(t, harness.Addr(t, 56580), client)

	for _, tc := range []struct {
		name string
		age  time.Duration
		drop bool
	}{
		{"old", 60 * time.Second, true},
		{"new", -60 * time.Second, true},
		{"recent", 10 * time.Second, false},
		{"slightly ahead", -10 * time.Second, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			p := isclient.NewISPacket(sentPacket(client, "stamped "+tc.name), isclient.Meta{})
			p.ReceivedTime = uint64(time.Now().Add(-tc.age).UnixMilli())
			if err := tx.SendISPacket(p); err != nil {
				t.Fatal(err)
			}
			if err := tx.SendLine(sentPacket(client, "helper "+tc.name)); err != nil {
				t.Fatal(err)
			}

			packet := passedPacket(client, "stamped "+tc.name)
			helper := passedPacket(client, "helper "+tc.name)
			if tc.drop {
				readISPacket(t, rx, helper)
				return
			}
			got := readISPacket(t, rx, packet)
			if got.GetReceivedTime() != p.ReceivedTime {
				t.Errorf("received_time %d, want %d", got.GetReceivedTime(), p.ReceivedTime)
			}
			readISPacket(t, rx, helper)
		})
	}

	s.WaitLog("seconds in the past, dropping such packets", 1, 5*time.Second)
	expectTotal(t, "is2_received_too_old", 1)
	expectTotal(t, "is2_received_too_new", 1)

	s.Stop()
}

// With IS2Received off, as on an aggregator, no entry is added, but the
// entries which come in are passed on, the newest five of them, and the
// packets are still stamped.
func TestIS2ReceivedDisabled(t *testing.T) {
	harness.Parallel(t)
	harness.RequireIS2(t)
	s := harness.StartText(t, "is2-received-off", is2BasicWith(t, nil, "IS2Received false"))

	const plain = "N0PLAIN"
	const client = "N0IS2"
	rx := isclient.ConnectIS2(t, harness.Addr(t, 56152), "N1RX")
	txPlain := isclient.Connect(t, harness.Addr(t, 55580), plain)
	tx := isclient.ConnectIS2(t, harness.Addr(t, 56580), client)

	t.Run("plaintext client", func(t *testing.T) {
		txPlain.Send(t, sentPacket(plain, "from a plaintext client"))
		got := readISPacket(t, rx, passedPacket(plain, "from a plaintext client"))
		checkReceived(t, "client", got, nil)
		checkStamp(t, "client", got)
	})

	t.Run("six entries", func(t *testing.T) {
		p := isclient.NewISPacket(sentPacket(client, "with six entries"), isclient.Meta{})
		for i := 1; i <= 6; i++ {
			p.Received = append(p.Received, received(fmt.Sprintf("S%d", i), fmt.Sprintf("U%d", i), aprsis2.VerificationStatus_WEAK, nil))
		}
		if err := tx.SendISPacket(p); err != nil {
			t.Fatal(err)
		}
		got := readISPacket(t, rx, passedPacket(client, "with six entries"))
		checkReceived(t, "client", got, p.Received[1:])
	})

	s.Stop()
}

// With IS2ReceivedAddr off, the entry of this server has no address, while
// the addresses in the entries which come in are passed on to the peers.
func TestIS2ReceivedNoAddr(t *testing.T) {
	harness.Parallel(t)
	harness.RequireIS2(t)

	udp := isclient.ListenIS2Peer(t, harness.AddrOn(t, "127.0.0.1", 16405), "TESTIN-2", is2PeerKeys, 2)
	udp.SetDestination(t, harness.AddrOn(t, "127.0.0.1", 16404))
	s := harness.StartText(t, "is2-received-noaddr", is2BasicWith(t, nil, "IS2ReceivedAddr false"))
	if err := udp.Negotiate(20 * time.Second); err != nil {
		t.Fatalf("failed to negotiate the IS2 corepeer link: %v", err)
	}

	const client = "N0IS2"
	tx := isclient.ConnectIS2(t, harness.Addr(t, 56580), client)

	p := isclient.NewISPacket(sentPacket(client, "no address of mine"), isclient.Meta{})
	p.Received = []*aprsis2.IS2Received{received("S1", "U1", aprsis2.VerificationStatus_STRONG, []byte{10, 0, 0, 1})}
	if err := tx.SendISPacket(p); err != nil {
		t.Fatal(err)
	}

	got := readISPacket(t, udp, passedPacket(client, "no address of mine"))
	checkReceived(t, "peer", got, []*aprsis2.IS2Received{
		received("S1", "U1", aprsis2.VerificationStatus_STRONG, []byte{10, 0, 0, 1}),
		received("TESTING", client, aprsis2.VerificationStatus_WEAK, nil),
	})

	s.Stop()
}
