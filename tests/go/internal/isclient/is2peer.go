package isclient

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/binary"
	"errors"
	"fmt"
	"net"
	"regexp"
	"strings"
	"testing"
	"time"

	"google.golang.org/protobuf/proto"

	"github.com/hessu/aprsc/tests/go/internal/aprsis2"
)

const (
	is2HMACMarker = 0x05

	// the largest UDP frame, with its signature, that is sent to a peer
	maxUDPFrame = 1400

	// a frame whose timestamp is off by more than this many seconds is
	// dropped by the receiver
	UDPTimestampWindow = 10
)

// IS2Peer is a UDP socket which plays an IS2 core peer of the server under
// test: it negotiates the IS2 upgrade, sends signed frames to the server's peer
// group address and receives the ones the server sends.
type IS2Peer struct {
	conn *net.UDPConn
	dest *net.UDPAddr

	// Callsign is the peer's own server id, used in the handshake.
	Callsign string
	// Keys are the shared secrets by key identifier, TxKey the one frames
	// are signed with. TxKey can be changed during a test.
	Keys  map[uint8]string
	TxKey uint8

	// handshake state: we have been told the peer does IS2 (rx), and we have
	// told the peer that we do (tx)
	okRx, okTx bool

	queue []*aprsis2.ISPacket
}

// ListenIS2Peer binds the UDP address local as a peer with the given server id
// and signing keys.
func ListenIS2Peer(t *testing.T, local, callsign string, keys map[uint8]string, txKey uint8) *IS2Peer {
	t.Helper()

	laddr, err := net.ResolveUDPAddr("udp", local)
	if err != nil {
		t.Fatalf("bad UDP address %q: %v", local, err)
	}
	conn, err := net.ListenUDP("udp", laddr)
	if err != nil {
		t.Fatalf("failed to bind UDP socket %s: %v", local, err)
	}
	t.Cleanup(func() { conn.Close() })

	conn.SetReadBuffer(32768)
	conn.SetWriteBuffer(32768)

	return &IS2Peer{conn: conn, Callsign: callsign, Keys: keys, TxKey: txKey}
}

// SetDestination sets the address of the server's peer group socket.
func (p *IS2Peer) SetDestination(t *testing.T, addr string) {
	t.Helper()

	daddr, err := net.ResolveUDPAddr("udp", addr)
	if err != nil {
		t.Fatalf("bad UDP address %q: %v", addr, err)
	}
	p.dest = daddr
}

// Sign appends the authentication trailer to a framed message: a marker, the
// key identifier, the time and an HMAC-SHA256 over the frame and those. The
// time can be given to produce frames with a wrong timestamp for testing.
func (p *IS2Peer) Sign(frame []byte, at time.Time) []byte {
	trailer := []byte{is2HMACMarker, p.TxKey, 0, 0, 0, 0}
	binary.BigEndian.PutUint32(trailer[2:], uint32(at.Unix()))

	mac := hmac.New(sha256.New, []byte(p.Keys[p.TxKey]))
	mac.Write(frame)
	mac.Write(trailer)

	out := append(append([]byte{}, frame...), trailer...)
	return mac.Sum(out)
}

// SendDatagram sends bytes to the server as one datagram, as they are.
func (p *IS2Peer) SendDatagram(b []byte) (int, error) {
	return p.conn.WriteToUDP(b, p.dest)
}

// SendFrameSigned signs a framed message and sends it.
func (p *IS2Peer) SendFrameSigned(frame []byte, at time.Time) error {
	signed := p.Sign(frame, at)
	if len(signed) > maxUDPFrame {
		return fmt.Errorf("attempted to write too large UDP frame: %d is over %d bytes", len(signed), maxUDPFrame)
	}
	_, err := p.SendDatagram(signed)
	return err
}

// SendLine sends one packet in a signed frame.
func (p *IS2Peer) SendLine(line string) error {
	return p.SendFrameSigned(FrameWrap(MarshalPackets(line)), time.Now())
}

// SignedPacketFrame returns a signed IS_PACKET frame with a given timestamp,
// without sending it.
func (p *IS2Peer) SignedPacketFrame(line string, at time.Time) []byte {
	return p.Sign(FrameWrap(MarshalPackets(line)), at)
}

var peerControlRe = regexp.MustCompile(`^# PEER2 v 2 (\S+) (\S+) vers (\S+) (\S+) token (\S+)$`)

// handleControl processes a "# PEER2" handshake datagram, replying to hellos.
func (p *IS2Peer) handleControl(msg string) error {
	m := peerControlRe.FindStringSubmatch(msg)
	if m == nil {
		return fmt.Errorf("unrecognized control message %q", msg)
	}

	operation, token := m[1], m[5]
	switch operation {
	case "ok":
		p.okRx = true
	case "hello":
		p.okTx = true
		reply := fmt.Sprintf("# PEER2 v 2 ok %s vers go-gotest 1.0 token %s", p.Callsign, token)
		if _, err := p.SendDatagram([]byte(reply)); err != nil {
			return err
		}
	default:
		return fmt.Errorf("unknown PEER2 operation %q", operation)
	}

	return nil
}

// Linked reports whether the IS2 upgrade has completed in both directions.
func (p *IS2Peer) Linked() bool {
	return p.okRx && p.okTx
}

// Negotiate does the IS2 upgrade handshake with the server, starting from
// scratch, and waits for it to complete. The hello is repeated, in case the
// server was not ready for the first one.
func (p *IS2Peer) Negotiate(timeout time.Duration) error {
	p.okRx, p.okTx = false, false

	end := time.Now().Add(timeout)
	hello := fmt.Sprintf("# PEER2 v 2 hello %s vers go-gotest 1.0 token hq736t7q63", p.Callsign)

	for !p.Linked() {
		if time.Now().After(end) {
			return errors.New("IS2 corepeer negotiation timed out")
		}

		if !p.okRx {
			if _, err := p.SendDatagram([]byte(hello)); err != nil {
				return err
			}
		}

		// Handshake and frame errors while waiting are not fatal. Wait up to
		// two seconds for the next handshake datagram before repeating the
		// hello.
		stop := time.Now().Add(2 * time.Second)
		for !p.Linked() && time.Now().Before(stop) {
			p.readFrame(time.Until(stop))
		}
	}

	return nil
}

// readFrame reads datagrams until a data frame is found, handling handshake
// datagrams on the way. A datagram which is not a valid frame is an error.
func (p *IS2Peer) readFrame(timeout time.Duration) (*aprsis2.IS2Message, error) {
	end := time.Now().Add(timeout)

	for {
		msg, err := readDatagramRaw(p.conn, time.Until(end))
		if err != nil {
			return nil, err
		}

		if len(msg) >= 2 && string(msg[:2]) == "# " {
			if err := p.handleControl(strings.TrimRight(string(msg), "\r\n")); err != nil {
				return nil, err
			}
			continue
		}

		return p.decodeFrame(msg)
	}
}

func readDatagramRaw(conn *net.UDPConn, timeout time.Duration) ([]byte, error) {
	if err := conn.SetReadDeadline(time.Now().Add(timeout)); err != nil {
		return nil, err
	}
	buf := make([]byte, maxDatagram)
	n, _, err := conn.ReadFromUDP(buf)
	if err != nil {
		return nil, err
	}
	return buf[:n], nil
}

// decodeFrame verifies the signature of a UDP frame and decodes it.
func (p *IS2Peer) decodeFrame(msg []byte) (*aprsis2.IS2Message, error) {
	if len(msg) < 6 {
		return nil, errors.New("IS2 UDP frame too short")
	}
	if msg[0] != is2STX {
		return nil, errors.New("IS2 UDP frame does not start with STX")
	}

	length := int(msg[1])<<16 | int(msg[2])<<8 | int(msg[3])
	if len(msg) < length+5 {
		return nil, errors.New("IS2 UDP frame too short for message length")
	}
	if msg[4+length] != is2ETX {
		return nil, errors.New("IS2 UDP frame does not end with ETX")
	}

	frame, trailer := msg[:length+5], msg[length+5:]
	if err := p.checkSignature(frame, trailer); err != nil {
		return nil, err
	}

	m := &aprsis2.IS2Message{}
	if err := proto.Unmarshal(msg[4:4+length], m); err != nil {
		return nil, fmt.Errorf("failed to decode IS2Message: %w", err)
	}

	return m, nil
}

// checkSignature verifies the trailer of a frame: the marker, a known key, the
// HMAC (which covers the marker, key id and timestamp too), and a timestamp
// within the window.
func (p *IS2Peer) checkSignature(frame, trailer []byte) error {
	if len(trailer) != 6+sha256.Size || trailer[0] != is2HMACMarker {
		return errors.New("IS2 UDP frame has an invalid authentication trailer")
	}

	key, ok := p.Keys[trailer[1]]
	if !ok {
		return fmt.Errorf("IS2 UDP frame signed with unknown key %d", trailer[1])
	}

	mac := hmac.New(sha256.New, []byte(key))
	mac.Write(frame)
	mac.Write(trailer[:6])
	if !hmac.Equal(mac.Sum(nil), trailer[6:]) {
		return errors.New("IS2 UDP frame signature incorrect")
	}

	age := time.Now().Unix() - int64(binary.BigEndian.Uint32(trailer[2:6]))
	if age > UDPTimestampWindow || age < -UDPTimestampWindow {
		return fmt.Errorf("IS2 UDP frame timestamp is off by %d seconds", age)
	}

	return nil
}

// ReadPackets reads the next IS_PACKET frame and returns the data of its
// packets.
func (p *IS2Peer) ReadPackets(timeout time.Duration) ([]string, error) {
	end := time.Now().Add(timeout)

	for {
		m, err := p.readFrame(time.Until(end))
		if err != nil {
			return nil, err
		}
		if m.GetType() == aprsis2.IS2Message_KEEPALIVE_PING {
			continue
		}
		if m.GetType() != aprsis2.IS2Message_IS_PACKET {
			return nil, fmt.Errorf("wrong type of frame received: %v", m.GetType())
		}

		var out []string
		for _, ip := range m.GetIsPacket() {
			out = append(out, string(ip.GetIsPacketData()))
		}
		return out, nil
	}
}

// ReadPacket returns the data of the next packet.
func (p *IS2Peer) ReadPacket(timeout time.Duration) (string, error) {
	if len(p.queue) == 0 {
		end := time.Now().Add(timeout)
		for len(p.queue) == 0 {
			m, err := p.readFrame(time.Until(end))
			if err != nil {
				return "", err
			}
			if m.GetType() == aprsis2.IS2Message_IS_PACKET {
				p.queue = m.GetIsPacket()
			}
		}
	}

	ip := p.queue[0]
	p.queue = p.queue[1:]
	return string(ip.GetIsPacketData()), nil
}
