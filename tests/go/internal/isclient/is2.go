package isclient

import (
	"bufio"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math/rand"
	"net"
	"strings"
	"testing"
	"time"

	"google.golang.org/protobuf/proto"

	"github.com/hessu/aprsc/tests/go/internal/aprsis2"
)

// IS2 framing: STX, a 24-bit big-endian length, the message, ETX. On TCP the
// server only accepts frames up to 65535 bytes (the high byte of the length
// must be zero).
const (
	is2STX = 0x02
	is2ETX = 0x03

	is2AppName    = "IS2"
	is2AppVersion = "0.01"

	// is2Timeout is how long the login handshake and other request/reply
	// exchanges wait for the server.
	is2Timeout = 5 * time.Second
)

// FrameWrap wraps an encoded IS2Message in an IS2 frame.
func FrameWrap(body []byte) []byte {
	frame := make([]byte, 0, len(body)+5)
	frame = append(frame, is2STX, byte(len(body)>>16), byte(len(body)>>8), byte(len(body)))
	frame = append(frame, body...)
	return append(frame, is2ETX)
}

// MarshalPackets encodes an IS_PACKET message carrying the packet lines.
func MarshalPackets(lines ...string) []byte {
	var packets []*aprsis2.ISPacket
	for _, l := range lines {
		packets = append(packets, &aprsis2.ISPacket{
			Type:         aprsis2.ISPacket_IS_PACKET,
			IsPacketData: []byte(l),
		})
	}
	return marshalISPackets(packets)
}

func marshalISPackets(packets []*aprsis2.ISPacket) []byte {
	b, err := proto.Marshal(&aprsis2.IS2Message{
		Type:     aprsis2.IS2Message_IS_PACKET,
		IsPacket: packets,
	})
	if err != nil {
		panic(err) // a message built from valid parts always marshals
	}
	return b
}

// LoginError is a login which the server answered with a failure, or with a
// reply which does not say that a given passcode was verified.
type LoginError struct {
	Reply *aprsis2.IS2LoginReply
	Msg   string
}

func (e *LoginError) Error() string {
	return fmt.Sprintf("login failed: %s (result %v, code %v, message %q)",
		e.Msg, e.Reply.GetResult(), e.Reply.GetResultCode(), e.Reply.GetResultMessage())
}

// IS2 is a client of the IS2 protocol over TCP or SCTP.
type IS2 struct {
	conn net.Conn
	r    *bufio.Reader

	Callsign   string
	Signature  *aprsis2.IS2ServerSignature
	LoginReply *aprsis2.IS2LoginReply
	// Preamble has the plaintext lines received before an in-band upgrade
	// to IS2, up to and including the server's answer to the hello.
	Preamble []string

	// the packet type the receiver expects; duplicates for a dupe feed
	wantType aprsis2.ISPacket_Type

	queue  []*aprsis2.ISPacket
	closed bool
}

// WithAppName sets the application name in the IS2 login request.
func WithAppName(name string) Option { return func(c *config) { c.appName = name } }

// WithDuplicates makes an IS2 client expect duplicate packets (on a dupe
// feed port) instead of normal ones.
func WithDuplicates() Option { return func(c *config) { c.dupes = true } }

// DialIS2 connects to an IS2 port, retrying for a few seconds while the port is
// not open, and logs in. If the server rejects the login, the error is a
// *LoginError. If the login works but the filter given with WithFilter is
// refused, it returns both the connected client and an error.
func DialIS2(addr, callsign string, opts ...Option) (*IS2, error) {
	c, cfg, err := dialIS2(addr, callsign, opts)
	if err != nil {
		return nil, err
	}

	return c.loginResult(c.login(cfg))
}

// loginResult returns the client after a login, or nil if the login failed
// and the connection was closed.
func (c *IS2) loginResult(err error) (*IS2, error) {
	if err != nil && c.closed {
		return nil, err
	}
	return c, err
}

// dialIS2 connects to a TCP port, or an SCTP one with WithSCTP, retrying for
// a few seconds while the port is not open.
func dialIS2(addr, callsign string, opts []Option) (*IS2, config, error) {
	cfg := config{passcode: fmt.Sprint(passcodeFor(callsign)), appName: is2AppName}
	for _, o := range opts {
		o(&cfg)
	}

	var conn net.Conn
	var err error
	deadline := time.Now().Add(connectRetry)
	for {
		conn, err = cfg.dialConn(addr, time.Second)
		if err == nil {
			break
		}
		if errors.Is(err, ErrNoSCTP) || time.Now().After(deadline) {
			return nil, cfg, fmt.Errorf("failed to connect to %s: %w", addr, err)
		}
		time.Sleep(100 * time.Millisecond)
	}

	c := &IS2{
		conn:     conn,
		r:        bufio.NewReaderSize(conn, 70000),
		Callsign: callsign,
		wantType: aprsis2.ISPacket_IS_PACKET,
	}
	if cfg.dupes {
		c.wantType = aprsis2.ISPacket_IS_PACKET_DUPLICATE
	}

	return c, cfg, nil
}

// ErrNoUpgrade is returned by DialIS2Upgrade when the server logged the
// client in on plaintext instead of upgrading the connection to IS2.
var ErrNoUpgrade = errors.New("the server did not upgrade the connection to IS2")

// UpgradeHello is the line a client sends to upgrade a plaintext connection
// to IS2.
const UpgradeHello = "#is2 hello v=1"

// WithHello sets the hello line sent by DialIS2Upgrade, instead of
// UpgradeHello.
func WithHello(line string) Option { return func(c *config) { c.hello = line } }

// DialIS2Upgrade connects to a plaintext port and upgrades the connection to
// IS2 in-band: it sends the hello and a plaintext login line right away, as a
// client would to fall back on plaintext on a server which does not know of
// IS2, reads the plaintext lines up to the server's answer to the hello into
// Preamble, and logs in with IS2. If the server logs the client in on
// plaintext instead, the error is ErrNoUpgrade, and the connection is closed.
func DialIS2Upgrade(addr, callsign string, opts ...Option) (*IS2, error) {
	c, cfg, err := dialIS2(addr, callsign, opts)
	if err != nil {
		return nil, err
	}

	hello := cfg.hello
	if hello == "" {
		hello = UpgradeHello
	}
	login := fmt.Sprintf("user %s pass %s vers %s %s", callsign, cfg.passcode, cfg.appName, is2AppVersion)
	text := []string{hello + "\r\n" + login + "\r\n"}
	if cfg.slowUpgrade {
		// line terminators split, in separate reads
		text = []string{hello + "\r", "\n" + login + "\r", "\n"}
	}
	for _, s := range text {
		if err := c.SendRaw([]byte(s)); err != nil {
			c.conn.Close()
			return nil, err
		}
		if cfg.slowUpgrade {
			time.Sleep(slowUpgradeDelay)
		}
	}

	for {
		line, err := c.readTextLine(is2Timeout)
		if err != nil {
			c.conn.Close()
			return nil, fmt.Errorf("no answer to the IS2 hello: %w (got %q)", err, c.Preamble)
		}
		c.Preamble = append(c.Preamble, line)
		if line == "#is2 hello" || strings.HasPrefix(line, "#is2 hello ") {
			break
		}
		if strings.HasPrefix(line, "# logresp") {
			c.Close()
			return c, ErrNoUpgrade
		}
	}

	if cfg.slowUpgrade {
		// plaintext lines the server must still skip after the switch,
		// including an empty one
		if err := c.SendRaw([]byte("# a comment after the hello\r\n\r\n# another\n")); err != nil {
			c.Close()
			return nil, err
		}
		time.Sleep(slowUpgradeDelay)
	}

	return c.loginResult(c.login(cfg))
}

// slowUpgradeDelay separates the writes of WithSlowUpgrade, so that the
// server reads them separately.
const slowUpgradeDelay = 200 * time.Millisecond

// WithSlowUpgrade makes DialIS2Upgrade send the plaintext lines in pieces,
// with the line terminators split in separate reads, comment lines after the
// server has switched to IS2, and the STX of the first frame alone.
func WithSlowUpgrade() Option { return func(c *config) { c.slowUpgrade = true } }

// ConnectIS2Upgrade is like DialIS2Upgrade, but fails the test if it cannot
// connect, upgrade and log in. The connection is closed when the test
// finishes.
func ConnectIS2Upgrade(t *testing.T, addr, callsign string, opts ...Option) *IS2 {
	t.Helper()

	c, err := DialIS2Upgrade(addr, callsign, opts...)
	if c != nil {
		t.Cleanup(func() { c.Close() })
	}
	if err != nil {
		t.Fatalf("%s: IS2 upgrade and login to %s failed: %v", callsign, addr, err)
	}

	return c
}

// readTextLine reads a plaintext line, before an upgrade to IS2.
func (c *IS2) readTextLine(timeout time.Duration) (string, error) {
	if err := c.conn.SetReadDeadline(time.Now().Add(timeout)); err != nil {
		return "", err
	}
	line, err := c.r.ReadString('\n')
	if err != nil {
		return "", err
	}
	return strings.TrimRight(line, "\r\n"), nil
}

// login does the IS2 login: the server sends its signature first, then the
// client its login request, and the server answers it. If the server rejects
// the login, the error is a *LoginError. If the login works but the filter
// given with WithFilter is refused, it returns an error, and the client stays
// connected.
func (c *IS2) login(cfg config) error {
	callsign := c.Callsign

	// the server sends its signature first
	m, err := c.ReadFrame(is2Timeout)
	if err != nil {
		c.Close()
		return fmt.Errorf("failed to read the server signature: %w", err)
	}
	if m.GetType() != aprsis2.IS2Message_SERVER_SIGNATURE || m.GetServerSignature() == nil {
		c.Close()
		return fmt.Errorf("wrong type of message received instead of SERVER_SIGNATURE: %v", m.GetType())
	}
	c.Signature = m.GetServerSignature()

	req := &aprsis2.IS2Message{
		Type: aprsis2.IS2Message_LOGIN_REQUEST,
		LoginRequest: &aprsis2.IS2LoginRequest{
			Username:   callsign,
			Password:   cfg.passcode,
			AppName:    cfg.appName,
			AppVersion: is2AppVersion,
		},
	}
	if cfg.slowUpgrade {
		// the STX alone, then the rest of the frame
		var b []byte
		b, err = proto.Marshal(req)
		if err == nil {
			frame := FrameWrap(b)
			if err = c.SendRaw(frame[:1]); err == nil {
				time.Sleep(slowUpgradeDelay)
				err = c.SendRaw(frame[1:])
			}
		}
	} else {
		err = c.SendMessage(req)
	}
	if err != nil {
		c.Close()
		return err
	}

	// the server may send a keepalive ping before the reply
	m, err = c.readNonPing(is2Timeout)
	if err != nil {
		c.Close()
		return fmt.Errorf("no LOGIN_REPLY received: %w", err)
	}
	if m.GetType() != aprsis2.IS2Message_LOGIN_REPLY || m.GetLoginReply() == nil {
		c.Close()
		return fmt.Errorf("wrong type of response received for LOGIN_REQUEST: %v", m.GetType())
	}

	c.LoginReply = m.GetLoginReply()
	if c.LoginReply.GetResult() != aprsis2.IS2LoginReply_OK {
		c.Close()
		return &LoginError{Reply: c.LoginReply, Msg: "rejected"}
	}
	if cfg.passcode != "-1" && c.LoginReply.GetVerified() < aprsis2.VerificationStatus_WEAK {
		c.Close()
		return &LoginError{Reply: c.LoginReply, Msg: "not verified"}
	}

	if cfg.filter != "" {
		if err := c.SetFilter(cfg.filter); err != nil {
			return err
		}
	}

	return nil
}

// ConnectIS2 is like DialIS2, but fails the test if it cannot connect and log
// in. The connection is closed when the test finishes.
func ConnectIS2(t *testing.T, addr, callsign string, opts ...Option) *IS2 {
	t.Helper()

	c, err := DialIS2(addr, callsign, opts...)
	if c != nil {
		t.Cleanup(func() { c.Close() })
	}
	if err != nil {
		t.Fatalf("%s: IS2 login to %s failed: %v", callsign, addr, err)
	}

	return c
}

// WaitSignature waits for a SERVER_SIGNATURE frame, which the server sends to
// a connected client again after a live upgrade.
func (c *IS2) WaitSignature(timeout time.Duration) error {
	m, err := c.ReadFrame(timeout)
	if err != nil {
		return err
	}
	if m.GetType() != aprsis2.IS2Message_SERVER_SIGNATURE || m.GetServerSignature() == nil {
		return fmt.Errorf("wrong type of message received instead of SERVER_SIGNATURE: %v", m.GetType())
	}

	c.Signature = m.GetServerSignature()
	return nil
}

// Close closes the connection.
func (c *IS2) Close() error {
	c.closed = true
	return c.conn.Close()
}

// Disconnected reports whether the connection has been closed, by us or by
// the server.
func (c *IS2) Disconnected() bool {
	return c.closed
}

// SendRaw writes bytes to the connection as they are.
func (c *IS2) SendRaw(b []byte) error {
	_, err := c.conn.Write(b)
	return err
}

// SendMessage encodes and sends an IS2Message in a frame.
func (c *IS2) SendMessage(m *aprsis2.IS2Message) error {
	b, err := proto.Marshal(m)
	if err != nil {
		return err
	}
	return c.SendFrame(b)
}

// SendFrame wraps an encoded message in a frame and sends it.
func (c *IS2) SendFrame(body []byte) error {
	if len(body) >= 1<<24 {
		return fmt.Errorf("attempted to write too large frame: %d bytes", len(body))
	}
	return c.SendRaw(FrameWrap(body))
}

// SendLine sends one packet.
func (c *IS2) SendLine(line string) error {
	return c.SendPackets(line)
}

// SendPackets sends the packets in one frame.
func (c *IS2) SendPackets(lines ...string) error {
	return c.SendFrame(MarshalPackets(lines...))
}

// SendISPacket sends a packet with its optional fields (RSSI, SNR).
func (c *IS2) SendISPacket(p *aprsis2.ISPacket) error {
	return c.SendFrame(marshalISPackets([]*aprsis2.ISPacket{p}))
}

// ReadFrame reads one IS2 frame within timeout. A framing error closes the
// connection, as does the server closing it.
func (c *IS2) ReadFrame(timeout time.Duration) (*aprsis2.IS2Message, error) {
	if c.closed {
		return nil, errors.New("connection is closed")
	}
	if err := c.conn.SetReadDeadline(time.Now().Add(timeout)); err != nil {
		return nil, err
	}

	hdr, err := c.r.Peek(4)
	if err != nil {
		return nil, c.readError(err)
	}
	if hdr[0] != is2STX || hdr[1] != 0 {
		c.Close()
		return nil, errors.New("IS2 frame does not start with STX and a zero reserved byte")
	}
	length := int(binary.BigEndian.Uint16(hdr[2:4]))

	frame, err := c.r.Peek(length + 5)
	if err != nil {
		return nil, c.readError(err)
	}
	if frame[4+length] != is2ETX {
		c.Close()
		return nil, errors.New("IS2 frame does not end with ETX")
	}

	m := &aprsis2.IS2Message{}
	err = proto.Unmarshal(frame[4:4+length], m)
	c.r.Discard(length + 5)
	if err != nil {
		return nil, fmt.Errorf("failed to decode IS2Message: %w", err)
	}

	return m, nil
}

// readError marks the connection closed unless err is only a timeout.
func (c *IS2) readError(err error) error {
	if !IsTimeout(err) {
		c.closed = true
		if errors.Is(err, io.EOF) {
			return fmt.Errorf("connection closed by the server: %w", err)
		}
	}
	return err
}

// readNonPing reads frames until one that is not a ping request, replying to
// ping requests on the way.
func (c *IS2) readNonPing(timeout time.Duration) (*aprsis2.IS2Message, error) {
	end := time.Now().Add(timeout)

	for {
		m, err := c.ReadFrame(time.Until(end))
		if err != nil {
			return nil, err
		}
		if m.GetType() != aprsis2.IS2Message_KEEPALIVE_PING {
			return m, nil
		}

		ping := m.GetKeepalivePing()
		if ping == nil || ping.GetPingType() != aprsis2.IS2KeepalivePing_REQUEST {
			return nil, errors.New("unexpected ping frame received")
		}
		err = c.SendMessage(&aprsis2.IS2Message{
			Type: aprsis2.IS2Message_KEEPALIVE_PING,
			KeepalivePing: &aprsis2.IS2KeepalivePing{
				PingType:    aprsis2.IS2KeepalivePing_REPLY,
				RequestId:   ping.GetRequestId(),
				RequestData: ping.GetRequestData(),
			},
		})
		if err != nil {
			return nil, err
		}
	}
}

// ReadISPackets reads the next IS_PACKET frame and returns its packets.
func (c *IS2) ReadISPackets(timeout time.Duration) ([]*aprsis2.ISPacket, error) {
	m, err := c.readNonPing(timeout)
	if err != nil {
		return nil, err
	}
	if m.GetType() != aprsis2.IS2Message_IS_PACKET {
		return nil, fmt.Errorf("wrong type of frame received: %v", m.GetType())
	}
	if len(m.GetIsPacket()) == 0 {
		return nil, errors.New("IS_PACKET frame with no packets")
	}

	for _, p := range m.GetIsPacket() {
		if p.GetType() != c.wantType {
			return nil, fmt.Errorf("ISPacket type %v not expected", p.GetType())
		}
	}

	return m.GetIsPacket(), nil
}

// ReadPackets reads the next IS_PACKET frame and returns the data of its
// packets. Packets left over from an earlier ReadPacket come first.
func (c *IS2) ReadPackets(timeout time.Duration) ([]string, error) {
	var out []string

	if len(c.queue) == 0 {
		ps, err := c.ReadISPackets(timeout)
		if err != nil {
			return nil, err
		}
		c.queue = ps
	}

	for _, p := range c.queue {
		out = append(out, string(p.GetIsPacketData()))
	}
	c.queue = nil

	return out, nil
}

// ReadISPacket returns the next packet, with its optional fields.
func (c *IS2) ReadISPacket(timeout time.Duration) (*aprsis2.ISPacket, error) {
	if len(c.queue) == 0 {
		ps, err := c.ReadISPackets(timeout)
		if err != nil {
			return nil, err
		}
		c.queue = ps
	}

	p := c.queue[0]
	c.queue = c.queue[1:]
	return p, nil
}

// ReadPacket returns the data of the next packet.
func (c *IS2) ReadPacket(timeout time.Duration) (string, error) {
	p, err := c.ReadISPacket(timeout)
	if err != nil {
		return "", err
	}
	return string(p.GetIsPacketData()), nil
}

// Ping sends a keepalive ping and waits for the reply.
func (c *IS2) Ping() error {
	reqID := uint32(rand.Int31())

	err := c.SendMessage(&aprsis2.IS2Message{
		Type: aprsis2.IS2Message_KEEPALIVE_PING,
		KeepalivePing: &aprsis2.IS2KeepalivePing{
			PingType:  aprsis2.IS2KeepalivePing_REQUEST,
			RequestId: reqID,
		},
	})
	if err != nil {
		return err
	}

	end := time.Now().Add(2 * time.Second)
	for {
		m, err := c.ReadFrame(time.Until(end))
		if err != nil {
			return fmt.Errorf("no ping reply: %w", err)
		}
		if m.GetType() != aprsis2.IS2Message_KEEPALIVE_PING {
			continue
		}

		ping := m.GetKeepalivePing()
		if ping.GetPingType() != aprsis2.IS2KeepalivePing_REPLY {
			return fmt.Errorf("wrong type of ping frame received: %v", ping.GetPingType())
		}
		if ping.GetRequestId() != reqID {
			return fmt.Errorf("ping request id mismatch, sent %d, got %d", reqID, ping.GetRequestId())
		}
		return nil
	}
}

// SetFilter sets the filter with a PARAMETER_SET, and returns an error if the
// server does not apply it. The connection stays up in that case.
func (c *IS2) SetFilter(filter string) error {
	reqID := rand.Int31()

	err := c.SendMessage(&aprsis2.IS2Message{
		Type: aprsis2.IS2Message_PARAMETER,
		Parameter: &aprsis2.IS2Parameter{
			Type:         aprsis2.IS2Parameter_PARAMETER_SET,
			RequestId:    reqID,
			FilterString: filter,
		},
	})
	if err != nil {
		return err
	}

	m, err := c.ReadFrame(is2Timeout)
	if err != nil {
		return fmt.Errorf("no PARAMETER_APPLIED received: %w", err)
	}
	if m.GetType() != aprsis2.IS2Message_PARAMETER || m.GetParameter() == nil {
		return fmt.Errorf("wrong type of response received for PARAMETER_SET: %v", m.GetType())
	}

	rep := m.GetParameter()
	if rep.GetRequestId() != reqID {
		return fmt.Errorf("PARAMETER reply has wrong request id %d, expected %d", rep.GetRequestId(), reqID)
	}
	if rep.GetType() != aprsis2.IS2Parameter_PARAMETER_APPLIED {
		return fmt.Errorf("filter set reply: not applied (%v)", rep.GetType())
	}

	return nil
}

// Meta is the metadata an ISPacket carries about the reception of the
// packet. A nil pointer, or MODEM_UNDEFINED, is a field which is not set.
type Meta struct {
	RSSI, SNR *float32
	Modem     aprsis2.ISPacket_ModemType
	Bitrate   *uint32
	Frequency *float64
}

// NewISPacket builds an ISPacket with the given metadata.
func NewISPacket(data string, m Meta) *aprsis2.ISPacket {
	p := &aprsis2.ISPacket{
		Type:         aprsis2.ISPacket_IS_PACKET,
		IsPacketData: []byte(data),
		RxModem:      m.Modem,
	}
	if m.RSSI != nil {
		p.OptionalRxRssi = &aprsis2.ISPacket_RxRssi{RxRssi: *m.RSSI}
	}
	if m.SNR != nil {
		p.OptionalRxSnrDb = &aprsis2.ISPacket_RxSnrDb{RxSnrDb: *m.SNR}
	}
	if m.Bitrate != nil {
		p.OptionalRxBitrate = &aprsis2.ISPacket_RxBitrate{RxBitrate: *m.Bitrate}
	}
	if m.Frequency != nil {
		p.OptionalRxFrequency = &aprsis2.ISPacket_RxFrequency{RxFrequency: *m.Frequency}
	}
	return p
}

// MetaOf returns the metadata of an ISPacket.
func MetaOf(p *aprsis2.ISPacket) Meta {
	m := Meta{Modem: p.GetRxModem()}
	if v, ok := p.GetOptionalRxRssi().(*aprsis2.ISPacket_RxRssi); ok {
		m.RSSI = &v.RxRssi
	}
	if v, ok := p.GetOptionalRxSnrDb().(*aprsis2.ISPacket_RxSnrDb); ok {
		m.SNR = &v.RxSnrDb
	}
	if v, ok := p.GetOptionalRxBitrate().(*aprsis2.ISPacket_RxBitrate); ok {
		m.Bitrate = &v.RxBitrate
	}
	if v, ok := p.GetOptionalRxFrequency().(*aprsis2.ISPacket_RxFrequency); ok {
		m.Frequency = &v.RxFrequency
	}
	return m
}

// sameField compares two optional values: unset is only equal to unset.
func sameField[T comparable](a, b *T) bool {
	if a == nil || b == nil {
		return a == nil && b == nil
	}
	return *a == *b
}

// SamePacket reports whether two ISPackets have the same data and the same
// metadata, with unset fields only equal to unset ones.
func SamePacket(a, b *aprsis2.ISPacket) bool {
	if string(a.GetIsPacketData()) != string(b.GetIsPacketData()) {
		return false
	}

	am, bm := MetaOf(a), MetaOf(b)
	return am.Modem == bm.Modem &&
		sameField(am.RSSI, bm.RSSI) && sameField(am.SNR, bm.SNR) &&
		sameField(am.Bitrate, bm.Bitrate) && sameField(am.Frequency, bm.Frequency)
}

// ExpectISPacket expects the next packet from rx to be want, including its
// optional fields.
func ExpectISPacket(t *testing.T, rx *IS2, want *aprsis2.ISPacket) {
	t.Helper()

	got, err := rx.ReadISPacket(DefaultTimeout)
	if err != nil {
		t.Errorf("did not receive %q: %v", want.GetIsPacketData(), err)
		return
	}
	if !SamePacket(got, want) {
		t.Errorf("received packet differs:\n got  %v\n want %v", got, want)
	}
}
