package functional

import (
	"errors"
	"fmt"
	"math/rand"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
	"time"

	"github.com/hessu/aprsc/tests/go/internal/aprsis2"
	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// The IS2 tests need an aprsc built with IS2 enabled, which is not the
// default: ./configure --enable-is2, and APRSC_BIN pointing at the binary if
// it is not src/aprsc. Without it they are skipped.

// Simple IS2 login, and a ping. Port of t/80is2-login.t.
func TestIS2Login(t *testing.T) {
	harness.Parallel(t)
	harness.RequireIS2(t)
	s := harness.Start(t, "is2-basic")

	c := isclient.ConnectIS2(t, harness.Addr(t, 56152), "OH7LZB")

	if c.LoginReply.GetVerified() != aprsis2.VerificationStatus_WEAK {
		t.Errorf("login verification status %v, want WEAK", c.LoginReply.GetVerified())
	}
	if c.Signature.GetUsername() != "TESTING" {
		t.Errorf("server signature username %q, want TESTING", c.Signature.GetUsername())
	}
	if err := c.Ping(); err != nil {
		t.Errorf("ping failed: %v", err)
	}

	if err := c.Close(); err != nil {
		t.Errorf("failed to disconnect: %v", err)
	}

	s.Stop()
}

// Heavier packet load over IS2: 20 MB of packets in frames of about 1000
// bytes, in order, with a limited number of bytes outstanding. Port of
// t/81is2-load.t.
func TestIS2Load(t *testing.T) {
	harness.Parallel(t)
	harness.RequireIS2(t)
	s := harness.Start(t, "is2-basic")

	const loginTx = "N0GAT"
	rx := isclient.ConnectIS2(t, harness.Addr(t, 56152), "N1GAT")
	tx := isclient.ConnectIS2(t, harness.Addr(t, 56580), loginTx)

	time.Sleep(500 * time.Millisecond) // let it get started

	const flushInterval = 1000
	const byteLimit = 20 * 1024 * 1024
	const window = 64 * 1024

	var sent []string // packets sent, in order, not yet received
	var queue []string
	queueLen := 0
	outstanding, txn, rxn, txl, rxl, txf, rxf := 0, 0, 0, 0, 0, 0, 0
	wrong := 0

	received := func(packets []string) {
		for _, p := range packets {
			if p != sent[0] {
				if wrong++; wrong <= 5 {
					t.Errorf("received wrong packet: %q", p)
				}
			}
			sent = sent[1:]
			outstanding -= len(p)
			rxn++
			rxl += len(p)
		}
		rxf++
	}

	start := time.Now()

	for txl < byteLimit {
		p := fmt.Sprintf("M%d>APRS,qAR,%s:!6028.51N/02505.68E# packet %d %s END",
			txn%10000+10, loginTx, txn, strings.Repeat("A", rand.Intn(150)))
		sent = append(sent, p)
		queue = append(queue, p)
		queueLen += len(p)
		txl += len(p)
		txn++

		if queueLen >= flushInterval {
			if err := tx.SendPackets(queue...); err != nil {
				t.Fatalf("failed to send: %v", err)
			}
			outstanding += queueLen
			queue, queueLen = nil, 0
			txf++
		}

		for outstanding > window {
			packets, err := rx.ReadPackets(5 * time.Second)
			if err != nil {
				t.Fatalf("IS2 receiver failed: %v", err)
			}
			received(packets)
		}
	}

	if len(queue) > 0 {
		if err := tx.SendPackets(queue...); err != nil {
			t.Fatalf("failed to send: %v", err)
		}
		outstanding += queueLen
		txf++
	}

	t.Logf("reading the rest, have received %d packets, sent %d", rxn, txn)
	for outstanding > 0 {
		packets, err := rx.ReadPackets(5500 * time.Millisecond)
		if err != nil {
			t.Logf("read ended: %v", err)
			break
		}
		received(packets)
	}
	t.Logf("after reading the rest, have received %d packets, sent %d, outstanding %d bytes", rxn, txn, outstanding)

	dur := time.Since(start)
	t.Logf("took %.3f seconds, %.0f packets/sec, frames tx/rx %d/%d", dur.Seconds(), float64(rxn)/dur.Seconds(), txf, rxf)

	if rxn != txn {
		t.Errorf("received %d lines, sent %d", rxn, txn)
	}
	if rxl != txl {
		t.Errorf("received %d bytes, sent %d", rxl, txl)
	}
	if outstanding != 0 {
		t.Errorf("%d bytes outstanding in the server after timeout", outstanding)
	}

	s.Stop()
}

var is2PeerKeys = map[uint8]string{1: "firstkey", 2: "secondkey", 3: "thirdkey"}

// IS2 UDP core peers, with signed frames. While at it, check that the basic
// loop prevention rules work.
//
//  1. Traffic from upstreams goes to clients.
//  2. Traffic from core peers goes to clients.
//  3. Traffic does not pass between peers and upstreams.
//  4. Traffic from clients goes to core peers and upstreams.
//
// The testing order is selected so that the last packet proves the previous
// ones were not transmitted to the wrong sockets. Port of t/82is2-udp-peer.t.
func TestIS2UDPPeer(t *testing.T) {
	harness.Parallel(t)
	harness.RequireIS2(t)

	udp := isclient.ListenIS2Peer(t, harness.AddrOn(t, "127.0.0.1", 16405), "TESTIN-2", is2PeerKeys, 2)
	udp.SetDestination(t, harness.AddrOn(t, "127.0.0.1", 16404))
	// a second UDP peer in the same group, sharing the server's socket
	udp2 := isclient.ListenIS2Peer(t, harness.AddrOn(t, "127.0.0.1", 16406), "TESTIN-3", is2PeerKeys, 3)
	udp2.SetDestination(t, harness.AddrOn(t, "127.0.0.1", 16404))

	fake := isclient.ListenFakeUpstream(t, harness.AddrOn(t, "127.0.0.1", 10153), "FAKEUP")

	s := harness.Start(t, "is2-basic")

	client := isclient.Connect(t, harness.Addr(t, 55152), "N5CAL-1")
	up := fake.Accept(t)

	if err := udp.Negotiate(20 * time.Second); err != nil {
		t.Fatalf("failed to negotiate the IS2 corepeer link: %v", err)
	}
	if err := udp2.Negotiate(20 * time.Second); err != nil {
		t.Fatalf("failed to negotiate the IS2 corepeer link with the second peer: %v", err)
	}

	// Oversized datagrams, which do not fit in the server's input buffer,
	// must be dropped: with MSG_TRUNC, Linux reports the full length of a
	// truncated datagram. The traffic tests below verify that the server
	// survived them.
	big := []byte("# PEER2 v 2 hello TESTIN-2 " + strings.Repeat("x", 9000))
	if n, err := udp.SendDatagram(big); err != nil || n != len(big) {
		t.Fatalf("failed to send an oversized control datagram: %d, %v", n, err)
	}
	big = append([]byte{0x02, 0x00, 0x23, 0x10}, []byte(strings.Repeat("y", 9000))...)
	if n, err := udp.SendDatagram(big); err != nil || n != len(big) {
		t.Fatalf("failed to send an oversized IS2 datagram: %d, %v", n, err)
	}

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

	// 3b) from the second core peer to client
	const fromPeer2 = "SRC>DST,qAR,IGATE:core peer 2 to client"
	isclient.TxRx(t, udp2, client, fromPeer2, fromPeer2)

	// 4) from client to peers and upstreams
	const fromClient = "SRC>DST,qAR,IGATE:from client"
	isclient.TxRx(t, client, up, fromClient, fromClient)

	for name, peer := range map[string]*isclient.IS2Peer{"UDP peer": udp, "second UDP peer": udp2} {
		got, err := peer.ReadPackets(isclient.DefaultTimeout)
		if err != nil || len(got) != 1 || got[0] != fromClient {
			t.Errorf("packet from client to %s: got %q (%v), want [%q]", name, got, err, fromClient)
		}
	}

	// 5) frame authentication: the signature covers a timestamp, and a frame
	// whose timestamp is more than 10 seconds from the server's clock is
	// dropped, so that captured frames can not be replayed later. Frames
	// within the window pass. Both sides work in whole seconds, so stay a
	// second away from the limit.
	for _, offset := range []int{-12, 12, -8, 8} {
		t.Run(fmt.Sprintf("timestamp offset %d", offset), func(t *testing.T) {
			p := fmt.Sprintf("SRC>DST,qAR,IGATE:timestamp offset %d", offset)
			frame := isclient.FrameWrap(isclient.MarshalPackets(p))
			at := time.Now().Add(time.Duration(offset) * time.Second)
			if err := udp.SendFrameSigned(frame, at); err != nil {
				t.Fatalf("failed to send a frame with timestamp offset %d: %v", offset, err)
			}

			if offset == -12 || offset == 12 {
				// must be dropped: the helper packet must be the next one
				// received
				helper := fmt.Sprintf("SRC>DST,qAR,IGATE:helper after offset %d", offset)
				isclient.TxRx(t, udp, client, helper, helper)
			} else {
				got, err := client.ReadPacket(isclient.DefaultTimeout)
				if err != nil || got != p {
					t.Errorf("frame with timestamp offset %d: got %q (%v), want %q", offset, got, err, p)
				}
			}
		})
	}

	s.Stop()
}

// The forwarding of optional packet fields: RSSI and SNR. Port of
// t/83is2-optionals.t.
func TestIS2Optionals(t *testing.T) {
	harness.Parallel(t)
	harness.RequireIS2(t)
	s := harness.Start(t, "is2-basic")

	const loginTx = "N0GATE"
	const loginTxPlain = "N0GATE-2"
	tx := isclient.ConnectIS2(t, harness.Addr(t, 56580), loginTx)
	txPlain := isclient.Connect(t, harness.Addr(t, 55580), loginTxPlain)

	// a filter on the rx so that the helper packets get through
	rx := isclient.ConnectIS2(t, harness.Addr(t, 56580), "N1GATE", isclient.WithFilter("r/60.4752/25.0947/1"))

	const msgSrc = "M1SRC"
	rssi, snr := float32(42), float32(12)

	t.Run("IS2 to IS2", func(t *testing.T) {
		sent := isclient.NewISPacket(msgSrc+">APRS,OH2RDG*,WIDE,"+loginTx+",I:!6028.51N/02505.68E# should pass", &rssi, &snr)
		if err := tx.SendISPacket(sent); err != nil {
			t.Fatal(err)
		}
		isclient.ExpectISPacket(t, rx, isclient.NewISPacket(
			msgSrc+">APRS,OH2RDG*,WIDE,qAR,"+loginTx+":!6028.51N/02505.68E# should pass", &rssi, &snr))
	})

	// a position packet on the plain IS connection, with metadata
	t.Run("plain with RSSI and SNR", func(t *testing.T) {
		txPlain.Send(t, "#is2-meta rx_rssi=42 rx_snr_db=12")
		txPlain.Send(t, msgSrc+">APRS,OH2RDG*,WIDE,"+loginTxPlain+",I:!6028.51N/02505.68E# should pass 2")
		isclient.ExpectISPacket(t, rx, isclient.NewISPacket(
			msgSrc+">APRS,OH2RDG*,WIDE,qAR,"+loginTxPlain+":!6028.51N/02505.68E# should pass 2", &rssi, &snr))
	})

	// and with only the SNR
	t.Run("plain with SNR only", func(t *testing.T) {
		txPlain.Send(t, "#is2-meta rx_snr_db=12")
		txPlain.Send(t, msgSrc+">APRS,OH2RDG*,WIDE,"+loginTxPlain+",I:!6028.51N/02505.68E# should pass 3")
		isclient.ExpectISPacket(t, rx, isclient.NewISPacket(
			msgSrc+">APRS,OH2RDG*,WIDE,qAR,"+loginTxPlain+":!6028.51N/02505.68E# should pass 3", nil, &snr))
	})

	// A value which is not a number is ignored, with its key, and so is
	// one with anything after the number; the other key still applies.
	t.Run("plain with an unparsable RSSI", func(t *testing.T) {
		txPlain.Send(t, "#is2-meta rx_rssi=strong rx_snr_db=12")
		txPlain.Send(t, msgSrc+">APRS,OH2RDG*,WIDE,"+loginTxPlain+",I:!6028.51N/02505.68E# should pass 4")
		isclient.ExpectISPacket(t, rx, isclient.NewISPacket(
			msgSrc+">APRS,OH2RDG*,WIDE,qAR,"+loginTxPlain+":!6028.51N/02505.68E# should pass 4", nil, &snr))
	})
	t.Run("plain with trailing garbage in the SNR", func(t *testing.T) {
		txPlain.Send(t, "#is2-meta rx_rssi=42 rx_snr_db=12dB")
		txPlain.Send(t, msgSrc+">APRS,OH2RDG*,WIDE,"+loginTxPlain+",I:!6028.51N/02505.68E# should pass 5")
		isclient.ExpectISPacket(t, rx, isclient.NewISPacket(
			msgSrc+">APRS,OH2RDG*,WIDE,qAR,"+loginTxPlain+":!6028.51N/02505.68E# should pass 5", &rssi, nil))
	})

	s.Stop()
}

// Messaging over IS2 ports. Port of t/84is2-messaging.t.
func TestIS2Messaging(t *testing.T) {
	harness.Parallel(t)
	harness.RequireIS2(t)
	s := harness.Start(t, "is2-basic")

	const filter = "r/60.4752/25.0947/1"

	// A filter is set on both so that the helper packets get through.
	tx := isclient.ConnectIS2(t, harness.Addr(t, 56580), "N0GATE", isclient.WithFilter(filter))
	rx := isclient.ConnectIS2(t, harness.Addr(t, 56580), "N1GATE", isclient.WithFilter(filter))

	messaging(t, tx, rx, func() duplex {
		return isclient.ConnectIS2(t, harness.Addr(t, 56580), "N2GATE", isclient.WithFilter(filter))
	})

	s.Stop()
}

// Dropping of IS2 packets whose packet data would not be valid on a plaintext
// APRS-IS link. Port of t/85is2-drops.t.
func TestIS2Drops(t *testing.T) {
	harness.Parallel(t)
	harness.RequireIS2(t)
	s := harness.Start(t, "is2-basic")

	const loginTx = "N0GATE"
	tx := isclient.ConnectIS2(t, harness.Addr(t, 56580), loginTx)

	// The receiver is a plaintext full feed client, where an injected second
	// line would show up as a separate packet.
	rx := isclient.Connect(t, harness.Addr(t, 55152), "N1GATE")

	const msgSrc = "M1SRC"
	helper := msgSrc + ">APRS,OH2RDG*,WIDE," + loginTx + ",I:!6028.51N/02505.68E# helper"
	const inject = "INJECT>APRS,TCPIP*,qAC,T2FAKE:!6028.51N/02505.68E# injected"

	// A CR or LF in the packet data would split the packet in two lines on a
	// plaintext link, and the second line would bypass all validation.
	for _, sep := range []struct{ name, sep string }{{"CRLF", "\r\n"}, {"LF", "\n"}, {"CR", "\r"}} {
		t.Run("line end "+sep.name, func(t *testing.T) {
			isclient.ExpectDrop(t, tx, rx,
				msgSrc+">APRS,OH2RDG*,WIDE,"+loginTx+",I:>first"+sep.sep+inject, helper)
		})
	}

	// A client running a known-broken application is put in quirks mode,
	// where its packets go through a rewriting parser. IS2 packet data is not
	// terminated by a line end, so the parser must not look past its end:
	// packets with no '>' after the source call, or no ':' after the path.
	const loginQuirk = "N0QRK"
	quirk := isclient.ConnectIS2(t, harness.Addr(t, 56580), loginQuirk, isclient.WithAppName("HR-IXPWIND"))
	quirkHelper := loginQuirk + ">APRS,TCPIP*:!6028.51N/02505.68E# quirks helper"

	t.Run("quirks mode, no >", func(t *testing.T) {
		isclient.ExpectDropExact(t, quirk, rx, loginQuirk, quirkHelper)
	})
	t.Run("quirks mode, no :", func(t *testing.T) {
		isclient.ExpectDropExact(t, quirk, rx, loginQuirk+">APRS,WIDE", quirkHelper)
	})

	s.Stop()
}

// IS2 framing errors: the server must disconnect the client right away, must
// not process any further frames from it, and must survive. Port of
// t/86is2-framing.t.
func TestIS2Framing(t *testing.T) {
	harness.Parallel(t)
	harness.RequireIS2(t)
	s := harness.Start(t, "is2-basic")

	const login = "OH7LZB"

	// a valid message body, and the same body in a correct frame
	body := isclient.MarshalPackets(login + ">APRS,TCPIP*:>framing test")
	good := isclient.FrameWrap(body)

	frame := func(reserved byte, length int, etx byte) []byte {
		b := []byte{0x02, reserved, byte(length >> 8), byte(length)}
		return append(append(b, body...), etx)
	}

	// Broken frames, each followed by the correct one. The server must close
	// the connection on the broken frame, right away instead of waiting for
	// more data, and must not parse the following frame.
	cases := []struct {
		name string
		bad  []byte
	}{
		{"non-zero reserved byte", frame(0x01, len(body), 0x03)},
		{"too short content length", append(append([]byte{0x02, 0x00, 0x00, 0x02}, body[:2]...), 0x03)},
		{"too long content length", frame(0x00, 5000, 0x03)},
		{"missing ETX", frame(0x00, len(body), 0x04)},
	}

	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			is := isclient.ConnectIS2(t, harness.Addr(t, 56152), login)

			if err := is.SendRaw(append(append([]byte{}, c.bad...), good...)); err != nil {
				t.Fatalf("failed to send frames: %v", err)
			}

			if p, err := is.ReadPacket(3 * time.Second); err == nil {
				t.Errorf("received a packet %q after the broken frame", p)
			}
			if !is.Disconnected() {
				t.Errorf("server did not disconnect the client")
			}
		})
	}

	// the server must still be alive and accepting clients
	is := isclient.ConnectIS2(t, harness.Addr(t, 56152), login)
	if err := is.Close(); err != nil {
		t.Errorf("failed to disconnect from the server: %v", err)
	}

	s.Stop()
}

// The UDP peer signing keys follow a configuration reload: a key removed from
// the configuration must no longer be accepted. Port of t/87is2-key-reload.t.
func TestIS2KeyReload(t *testing.T) {
	harness.Parallel(t)
	harness.RequireIS2(t)

	// The server reloads its configuration file in place: the test writes
	// the changed configuration through the harness, which keeps the test's
	// ports in it.
	src, err := os.ReadFile(filepath.Join(harness.TestsDir(), "cfg-aprsc", "is2-basic"))
	if err != nil {
		t.Fatal(err)
	}
	withoutKey := func(key string) string {
		var out []string
		for _, l := range strings.Split(string(src), "\n") {
			f := strings.Fields(l)
			if len(f) > 1 && f[0] == "PeerGroupKey" && f[1] == key {
				continue
			}
			out = append(out, l)
		}
		return strings.Join(out, "\n")
	}

	// UDP peer socket, signing with key 2
	udp := isclient.ListenIS2Peer(t, harness.AddrOn(t, "127.0.0.1", 16405), "TESTIN-2", is2PeerKeys, 2)
	udp.SetDestination(t, harness.AddrOn(t, "127.0.0.1", 16404))

	s := harness.Start(t, "is2-basic")

	client := isclient.Connect(t, harness.Addr(t, 55152), "N5CAL-1")
	if err := udp.Negotiate(20 * time.Second); err != nil {
		t.Fatalf("failed to negotiate the IS2 corepeer link: %v", err)
	}

	// frames signed with key 2 pass before the reload
	const before = "SRC>DST,qAR,IGATE:signed with key 2 before reload"
	isclient.TxRx(t, udp, client, before, before)

	// remove key 2 from the configuration and reload
	const readOK = "New configuration read successfully"
	reloads := s.LogCount(readOK)
	s.ReplaceConfig(withoutKey("2"))
	s.Signal(syscall.SIGUSR1)
	s.WaitLog(readOK, reloads+1, 10*time.Second)

	// the peers are set up again on reload, so negotiate again
	if err := udp.Negotiate(20 * time.Second); err != nil {
		t.Fatalf("failed to negotiate the IS2 corepeer link after reload: %v", err)
	}

	// a frame signed with the removed key 2 must be dropped, key 1 still works
	if err := udp.SendLine("SRC>DST,qAR,IGATE:signed with key 2 after reload"); err != nil {
		t.Fatalf("failed to send a packet signed with the removed key: %v", err)
	}
	udp.TxKey = 1
	const after = "SRC>DST,qAR,IGATE:signed with key 1 after reload"
	if err := udp.SendLine(after); err != nil {
		t.Fatalf("failed to send a packet signed with key 1: %v", err)
	}
	got, err := client.ReadPacket(isclient.DefaultTimeout)
	if err != nil || got != after {
		t.Errorf("server forwarded a frame signed with a removed key, or dropped a valid one: got %q (%v), want %q", got, err, after)
	}

	// the server signs its own frames with key 1, which the peer still has
	const fromClient = "SRC>DST,qAR,IGATE:from client after reload"
	isclient.TxRx(t, client, udp, fromClient, fromClient)

	s.Stop()
}

// IS2 login rejections: the server must answer a rejected login with a
// LOGIN_REPLY saying so, instead of leaving the client waiting. Port of
// t/88is2-login-reject.t.
func TestIS2LoginReject(t *testing.T) {
	harness.Parallel(t)
	harness.RequireIS2(t)
	s := harness.Start(t, "is2-basic")

	// usernames which must be rejected, and why
	for _, r := range []struct{ call, why string }{
		{"ABCDEFGHIJKLMNOP", "too long"},
		{"OH7/LZB", "invalid characters"},
		{"pass", "built-in disallowed username"},
		{"LOGINA", "disallowed by configuration"},
		{serverCall, "our server id"},
	} {
		t.Run(r.why, func(t *testing.T) {
			c, err := isclient.DialIS2(harness.Addr(t, 56152), r.call)
			if c != nil {
				c.Close()
			}

			// the client must get a login reply with a failure, not a timeout
			var le *isclient.LoginError
			if !errors.As(err, &le) || le.Reply.GetResult() != aprsis2.IS2LoginReply_FAIL {
				t.Errorf("login as %q (%s) was not rejected with a login reply: %v", r.call, r.why, err)
			}
		})
	}

	// a valid login still works
	c := isclient.ConnectIS2(t, harness.Addr(t, 56152), "OH7LZB")
	c.Close()

	s.Stop()
}

// The IS2 output buffers are flushed at the end of the worker's round, so a
// packet reaches an IS2 client, and an IS2 peer, right away even after a burst
// which filled frames: it used to wait for the next frame to fill up, or for
// the next keepalive round up to two seconds later.
func TestIS2OutputFlush(t *testing.T) {
	harness.Parallel(t)
	harness.RequireIS2(t)

	udp := isclient.ListenIS2Peer(t, harness.AddrOn(t, "127.0.0.1", 16405), "TESTIN-2", is2PeerKeys, 2)
	udp.SetDestination(t, harness.AddrOn(t, "127.0.0.1", 16404))

	s := harness.Start(t, "is2-basic")

	const login = "N5CAL-1"
	tx := isclient.Connect(t, harness.Addr(t, 55580), login)
	rx := isclient.ConnectIS2(t, harness.Addr(t, 56152), "N5CAL-2")
	if err := udp.Negotiate(20 * time.Second); err != nil {
		t.Fatalf("failed to negotiate the IS2 corepeer link: %v", err)
	}

	// a burst, which goes out in frames of several packets
	const burst = 30
	burstPacket := func(i int) string {
		return fmt.Sprintf("SRC>DST,qAR,%s:burst packet %d", login, i)
	}
	for i := 0; i < burst; i++ {
		tx.Send(t, burstPacket(i))
	}
	for _, r := range []struct {
		name string
		rx   isclient.Receiver
	}{{"IS2 client", rx}, {"IS2 peer", udp}} {
		for i := 0; i < burst; i++ {
			got, err := r.rx.ReadPacket(isclient.DefaultTimeout)
			if err != nil || got != burstPacket(i) {
				t.Fatalf("%s: burst packet %d: got %q (%v)", r.name, i, got, err)
			}
		}
	}

	// A single packet after the burst must come out right away, not with
	// the next keepalive round.
	const lone = "SRC>DST,qAR," + login + ":lone packet after the burst"
	const limit = 500 * time.Millisecond
	start := time.Now()
	tx.Send(t, lone)
	for _, r := range []struct {
		name string
		rx   isclient.Receiver
	}{{"IS2 client", rx}, {"IS2 peer", udp}} {
		got, err := r.rx.ReadPacket(isclient.DefaultTimeout)
		if err != nil || got != lone {
			t.Errorf("%s: got %q (%v), want %q", r.name, got, err, lone)
		} else if d := time.Since(start); d > limit {
			t.Errorf("%s: the packet took %v to come out, limit %v", r.name, d, limit)
		}
	}

	s.Stop()
}

// IS2 clients can only set filters on ports which allow user filters: a full
// feed port must refuse the PARAMETER_SET and keep sending everything, while
// the igate port applies it. Port of t/89is2-filter-port.t.
func TestIS2FilterPort(t *testing.T) {
	harness.Parallel(t)
	harness.RequireIS2(t)
	s := harness.Start(t, "is2-basic")

	const loginTx = "N0GATE"
	tx := isclient.Connect(t, harness.Addr(t, 55580), loginTx)

	// an IS2 client on the full feed port: the login works, but the filter
	// request after it must be refused, and the connection must stay up
	full, err := isclient.DialIS2(harness.Addr(t, 56152), "N1FULL", isclient.WithFilter("r/60.4752/25.0947/1"))
	if full == nil {
		t.Fatalf("login on the full feed port failed: %v", err)
	}
	t.Cleanup(func() { full.Close() })
	if err == nil {
		t.Error("full feed port accepted a filter from an IS2 client")
	}
	if full.Disconnected() {
		t.Error("connection was lost after a refused filter")
	}
	if err := full.Ping(); err != nil {
		t.Errorf("connection does not work after a refused filter: %v", err)
	}

	// an IS2 client on the igate port, where filters are allowed
	igate := isclient.ConnectIS2(t, harness.Addr(t, 56580), "N1GATE", isclient.WithFilter("r/60.4752/25.0947/1"))

	// A packet far outside the filter range goes to the full feed client, and
	// the igate client gets only the one inside its range.
	const outside = "M1SRC>APRS,OH2RDG*,WIDE," + loginTx + ",I:!4000.00N/01000.00E# outside the filter"
	const outsideRx = "M1SRC>APRS,OH2RDG*,WIDE,qAR," + loginTx + ":!4000.00N/01000.00E# outside the filter"
	const inside = "M1SRC>APRS,OH2RDG*,WIDE," + loginTx + ",I:!6028.51N/02505.68E# inside the filter"
	isclient.TxRx(t, tx, full, outside, outsideRx)
	isclient.ExpectDropLiteral(t, tx, igate, outside, inside)

	s.Stop()
}

// A server built without IS2 (the default) rejects every IS2 configuration
// keyword, so that IS2 can not be enabled by configuration. Skipped when IS2 is
// enabled. Port of t/90is2-disabled.t.
func TestIS2Disabled(t *testing.T) {
	harness.Parallel(t)
	harness.RequireNoIS2(t)

	base, err := os.ReadFile(filepath.Join(harness.TestsDir(), "cfg-aprsc", "basic"))
	if err != nil {
		t.Fatal(err)
	}

	check := func(extra string) error {
		path := filepath.Join(t.TempDir(), "is2-disabled.cfg")
		if err := os.WriteFile(path, append(append([]byte{}, base...), []byte("\n"+extra+"\n")...), 0o644); err != nil {
			t.Fatal(err)
		}
		_, err := harness.CheckConfigFile(t, path)
		return err
	}

	// the control: the plain configuration is accepted
	if err := check(""); err != nil {
		t.Fatalf("the configuration without IS2 lines was rejected: %v", err)
	}

	for _, line := range []string{
		`Listen "IS2 full" is2fullfeed tcp 0.0.0.0 56152`,
		`Listen "IS2 igate" is2igate tcp 0.0.0.0 56580`,
		`Listen "IS2 dupes" is2dupefeed tcp 0.0.0.0 56153`,
		`Uplink is2up full tcp 127.0.0.1 10154 is2`,
		`PeerGroupKey 1 firstkey`,
		`PeerGroupKeyTransmit 1`,
	} {
		t.Run(line, func(t *testing.T) {
			if err := check(line); err == nil {
				t.Errorf("server accepted a configuration with IS2 enabled: %s", line)
			}
		})
	}
}
