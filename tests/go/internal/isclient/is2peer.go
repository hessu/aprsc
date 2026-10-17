package isclient

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"math/rand"
	"net"
	"slices"
	"strconv"
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
	// Versions is the comma-separated list of the UDP peering protocol
	// versions offered in the hello and accepted from the server: "2" if
	// not set.
	Versions string

	// handshake state: we have been told the peer does IS2 (rx), and we have
	// told the peer that we do (tx)
	okRx, okTx bool

	// data frames which arrived during the handshake, returned by the
	// reads which follow it
	pending []*aprsis2.IS2Message

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

// SendMessage sends a message in a signed frame.
func (p *IS2Peer) SendMessage(m *aprsis2.IS2Message) error {
	b, err := proto.Marshal(m)
	if err != nil {
		return err
	}
	return p.SendFrameSigned(FrameWrap(b), time.Now())
}

// replyPing answers a keepalive ping request.
func (p *IS2Peer) replyPing(ping *aprsis2.IS2KeepalivePing) error {
	return p.SendMessage(&aprsis2.IS2Message{
		Type: aprsis2.IS2Message_KEEPALIVE_PING,
		KeepalivePing: &aprsis2.IS2KeepalivePing{
			PingType:    aprsis2.IS2KeepalivePing_REPLY,
			RequestId:   ping.GetRequestId(),
			RequestData: ping.GetRequestData(),
		},
	})
}

// AwaitPing reads frames until a keepalive ping request from the server,
// replies to it, and returns the frame it came in. Other frames are dropped.
func (p *IS2Peer) AwaitPing(timeout time.Duration) (*aprsis2.IS2Message, error) {
	end := time.Now().Add(timeout)

	for {
		m, err := p.readFrame(time.Until(end))
		if err != nil {
			return nil, err
		}
		ping := m.GetKeepalivePing()
		if m.GetType() != aprsis2.IS2Message_KEEPALIVE_PING || ping.GetPingType() != aprsis2.IS2KeepalivePing_REQUEST {
			continue
		}
		return m, p.replyPing(ping)
	}
}

// Ping sends a keepalive ping request and waits for the reply to it,
// answering any ping requests of the server on the way.
func (p *IS2Peer) Ping(timeout time.Duration) error {
	reqID := uint32(rand.Int31())
	err := p.SendMessage(&aprsis2.IS2Message{
		Type: aprsis2.IS2Message_KEEPALIVE_PING,
		KeepalivePing: &aprsis2.IS2KeepalivePing{
			PingType:  aprsis2.IS2KeepalivePing_REQUEST,
			RequestId: reqID,
		},
	})
	if err != nil {
		return err
	}

	end := time.Now().Add(timeout)
	for {
		m, err := p.readFrame(time.Until(end))
		if err != nil {
			return fmt.Errorf("no ping reply: %w", err)
		}
		if m.GetType() != aprsis2.IS2Message_KEEPALIVE_PING {
			continue
		}
		ping := m.GetKeepalivePing()
		if ping.GetPingType() == aprsis2.IS2KeepalivePing_REQUEST {
			if err := p.replyPing(ping); err != nil {
				return err
			}
			continue
		}
		if ping.GetRequestId() != reqID {
			return fmt.Errorf("ping request id mismatch, sent %d, got %d", reqID, ping.GetRequestId())
		}
		return nil
	}
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

// PeerHelloToken is the token of the hellos sent by Negotiate.
const PeerHelloToken = "hq736t7q63"

// PeerControlPrefix starts the UDP peer handshake datagrams.
const PeerControlPrefix = "#is2 peer "

// PeerControl is a "#is2 peer" handshake datagram: its type (hello or ok) and
// its key=value fields.
type PeerControl struct {
	Type   string
	Fields map[string]string
	// Raw is the datagram without the line terminator.
	Raw string
}

// ParsePeerControl decodes a "#is2 peer" handshake datagram. Bare words after
// the type are skipped, like the server does.
func ParsePeerControl(msg string) (*PeerControl, error) {
	raw := strings.TrimRight(msg, "\r\n")
	rest, ok := strings.CutPrefix(raw, PeerControlPrefix)
	words := strings.Split(rest, " ")
	if !ok || words[0] == "" {
		return nil, fmt.Errorf("unrecognized control message %q", msg)
	}

	c := &PeerControl{Type: words[0], Fields: make(map[string]string), Raw: raw}
	for _, w := range words[1:] {
		if k, v, ok := strings.Cut(w, "="); ok && k != "" && v != "" {
			c.Fields[k] = v
		}
	}
	return c, nil
}

// PeerControlLine formats an unsigned "#is2 peer" handshake datagram, with
// the extra key=value pairs appended.
func PeerControlLine(typ, versions, id, token string, extra ...string) string {
	fields := []string{PeerControlPrefix + typ, "v=" + versions, "id=" + id, "vers=go-gotest/1.0", "token=" + token}
	return strings.Join(append(fields, extra...), " ")
}

// SignControl appends the signature to a handshake line: the key identifier,
// the time and an HMAC-SHA256 of the line up to those, made with the given
// key from Keys. The key and time can be given to produce invalid handshakes
// for testing.
func (p *IS2Peer) SignControl(line string, keyID uint8, at time.Time) string {
	signed := fmt.Sprintf("%s key=%d ts=%d", line, keyID, at.Unix())
	mac := hmac.New(sha256.New, []byte(p.Keys[keyID]))
	mac.Write([]byte(signed))
	return signed + " mac=" + hex.EncodeToString(mac.Sum(nil))
}

// ControlLine returns a handshake line of the peer, signed with TxKey.
func (p *IS2Peer) ControlLine(typ, versions, token string, extra ...string) string {
	return p.SignControl(PeerControlLine(typ, versions, p.Callsign, token, extra...), p.TxKey, time.Now())
}

// VerifyControl checks the signature of a handshake datagram from the
// server: a known key, the HMAC, and a time within the window.
func (p *IS2Peer) VerifyControl(c *PeerControl) error {
	i := strings.LastIndex(c.Raw, " mac=")
	if i < 0 || strings.Contains(c.Raw[i+1:], " ") {
		return fmt.Errorf("handshake without a signature at the end: %q", c.Raw)
	}
	signed, macHex := c.Raw[:i], c.Raw[i+5:]
	keyID, err := strconv.ParseUint(c.Fields["key"], 10, 8)
	if err != nil {
		return fmt.Errorf("handshake with an invalid key id: %q", c.Raw)
	}
	key, ok := p.Keys[uint8(keyID)]
	if !ok {
		return fmt.Errorf("handshake signed with an unknown key %d", keyID)
	}
	got, err := hex.DecodeString(macHex)
	if err != nil {
		return fmt.Errorf("handshake with an invalid mac: %q", c.Raw)
	}
	mac := hmac.New(sha256.New, []byte(key))
	mac.Write([]byte(signed))
	if !hmac.Equal(got, mac.Sum(nil)) {
		return fmt.Errorf("handshake with a wrong signature: %q", c.Raw)
	}
	ts, err := strconv.ParseInt(c.Fields["ts"], 10, 64)
	if err != nil {
		return fmt.Errorf("handshake with an invalid ts: %q", c.Raw)
	}
	if age := time.Now().Unix() - ts; age > UDPTimestampWindow || age < -UDPTimestampWindow {
		return fmt.Errorf("handshake timestamp off by %d seconds", age)
	}
	return nil
}

// pickVersion returns the highest version in the comma-separated list
// offered which is also in ours, or "" if there is none.
func pickVersion(offered, ours string) string {
	best, bestN := "", 0
	for _, v := range strings.Split(offered, ",") {
		n, err := strconv.Atoi(v)
		if err != nil {
			return ""
		}
		if n > bestN && slices.Contains(strings.Split(ours, ","), v) {
			best, bestN = v, n
		}
	}
	return best
}

// versions returns the protocol versions the peer offers and accepts.
func (p *IS2Peer) versions() string {
	if p.Versions == "" {
		return "2"
	}
	return p.Versions
}

// handleControl processes a "#is2 peer" handshake datagram, replying to hellos.
func (p *IS2Peer) handleControl(msg string) error {
	c, err := ParsePeerControl(msg)
	if err != nil {
		return err
	}
	if err := p.VerifyControl(c); err != nil {
		return err
	}

	switch c.Type {
	case "ok":
		if c.Fields["token"] != PeerHelloToken {
			return fmt.Errorf("peer ok with token %q, want %q", c.Fields["token"], PeerHelloToken)
		}
		p.okRx = true
	case "hello":
		v := pickVersion(c.Fields["v"], p.versions())
		if v == "" {
			return fmt.Errorf("peer hello without a common version: %q", msg)
		}
		p.okTx = true
		reply := p.ControlLine("ok", v, c.Fields["token"])
		if _, err := p.SendDatagram([]byte(reply)); err != nil {
			return err
		}
	default:
		return fmt.Errorf("unknown peer handshake operation %q", c.Type)
	}

	return nil
}

// ReadControl reads datagrams until a handshake datagram of the given type
// is received, without answering it, and returns it. Data frames and other
// handshake datagrams are skipped.
func (p *IS2Peer) ReadControl(typ string, timeout time.Duration) (*PeerControl, error) {
	end := time.Now().Add(timeout)

	for {
		msg, err := readDatagramRaw(p.conn, time.Until(end))
		if err != nil {
			return nil, err
		}
		if !strings.HasPrefix(string(msg), PeerControlPrefix) {
			continue
		}
		c, err := ParsePeerControl(string(msg))
		if err != nil {
			return nil, err
		}
		if c.Type == typ {
			return c, nil
		}
	}
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
	hello := p.ControlLine("hello", p.versions(), PeerHelloToken)

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
		// hello. One datagram at a time, so that the handshake is seen to
		// be complete as soon as its last datagram has been handled: the
		// server starts pinging soon after, and a frame which arrives
		// before that is kept for the reads which follow.
		stop := time.Now().Add(2 * time.Second)
		for !p.Linked() && time.Now().Before(stop) {
			if m, _ := p.readDatagram(time.Until(stop)); m != nil {
				p.pending = append(p.pending, m)
			}
		}
	}

	return nil
}

// readFrame reads datagrams until a data frame is found, handling handshake
// datagrams on the way. A datagram which is not a valid frame is an error.
func (p *IS2Peer) readFrame(timeout time.Duration) (*aprsis2.IS2Message, error) {
	if len(p.pending) > 0 {
		m := p.pending[0]
		p.pending = p.pending[1:]
		return m, nil
	}

	end := time.Now().Add(timeout)

	for {
		m, err := p.readDatagram(time.Until(end))
		if err != nil {
			return nil, err
		}
		if m != nil {
			return m, nil
		}
	}
}

// readDatagram reads one datagram. A handshake datagram is handled and
// returns a nil message; a data frame is decoded and returned. A datagram
// which is neither is an error.
func (p *IS2Peer) readDatagram(timeout time.Duration) (*aprsis2.IS2Message, error) {
	msg, err := readDatagramRaw(p.conn, timeout)
	if err != nil {
		return nil, err
	}

	if strings.HasPrefix(string(msg), PeerControlPrefix) {
		return nil, p.handleControl(strings.TrimRight(string(msg), "\r\n"))
	}

	return p.decodeFrame(msg)
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

// ReadISPacket returns the next packet with its metadata.
func (p *IS2Peer) ReadISPacket(timeout time.Duration) (*aprsis2.ISPacket, error) {
	if len(p.queue) == 0 {
		end := time.Now().Add(timeout)
		for len(p.queue) == 0 {
			m, err := p.readFrame(time.Until(end))
			if err != nil {
				return nil, err
			}
			if m.GetType() == aprsis2.IS2Message_IS_PACKET {
				p.queue = m.GetIsPacket()
			}
		}
	}

	ip := p.queue[0]
	p.queue = p.queue[1:]
	return ip, nil
}

// ReadPacket returns the data of the next packet.
func (p *IS2Peer) ReadPacket(timeout time.Duration) (string, error) {
	ip, err := p.ReadISPacket(timeout)
	if err != nil {
		return "", err
	}
	return string(ip.GetIsPacketData()), nil
}
