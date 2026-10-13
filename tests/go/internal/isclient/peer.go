package isclient

import (
	"bufio"
	"fmt"
	"net"
	"regexp"
	"strings"
	"testing"
	"time"

	fap "github.com/hessu/go-aprs-fap"
)

// acceptTimeout is how long Accept waits for the server under test to connect
// to the fake upstream.
const acceptTimeout = 15 * time.Second

// Peer is one end of a connection where the test plays the server, for
// example an upstream connection made by the server under test.
type Peer struct {
	conn net.Conn
	r    *bufio.Reader

	// User is the callsign the server logged in with.
	User string
}

// SendLine sends a line terminated with CRLF.
func (p *Peer) SendLine(line string) error {
	_, err := fmt.Fprintf(p.conn, "%s\r\n", line)
	return err
}

// ReadLine reads a line, without its CRLF, waiting up to timeout.
func (p *Peer) ReadLine(timeout time.Duration) (string, error) {
	if err := p.conn.SetReadDeadline(time.Now().Add(timeout)); err != nil {
		return "", err
	}
	line, err := p.r.ReadString('\n')
	if err != nil {
		return "", err
	}
	return strings.TrimRight(line, "\r\n"), nil
}

// ReadPacket reads lines until one that is not a comment is found.
func (p *Peer) ReadPacket(timeout time.Duration) (string, error) {
	for {
		line, err := p.ReadLine(timeout)
		if err != nil {
			return "", err
		}
		if !strings.HasPrefix(line, "#") {
			return line, nil
		}
	}
}

// Close closes the connection.
func (p *Peer) Close() error {
	return p.conn.Close()
}

// FakeUpstream listens for connections from the server under test, and plays
// a server which lets it log in.
type FakeUpstream struct {
	ln   *net.TCPListener
	call string
	// Comment lines which Accept sends in addition to the banner and the
	// logresp: AfterBanner right after the banner, before the server has
	// logged in, and BeforeLogresp after its login, where a keepalive
	// beacon would come. The server must skip them.
	AfterBanner, BeforeLogresp []string
}

// ListenFakeUpstream starts listening on addr (for example "127.0.0.1:54153"
// or "[::]:54153"). call is the server callsign sent in the logresp. Do it
// before starting the server, so that its first connection attempt succeeds.
func ListenFakeUpstream(t *testing.T, addr, call string) *FakeUpstream {
	t.Helper()

	ln, err := net.Listen("tcp", addr)
	if err != nil {
		t.Fatalf("failed to listen on %s: %v", addr, err)
	}
	t.Cleanup(func() { ln.Close() })

	return &FakeUpstream{ln: ln.(*net.TCPListener), call: call}
}

// Close stops listening. Connections already accepted stay open.
func (f *FakeUpstream) Close() error {
	return f.ln.Close()
}

var loginRe = regexp.MustCompile(`^user\s+(\S+)\s+pass\s+(\d+)`)

// Accept waits for the server to connect and log in, and answers the login
// like an APRS-IS server. The connection is closed when the test finishes.
func (f *FakeUpstream) Accept(t *testing.T) *Peer {
	t.Helper()

	f.ln.SetDeadline(time.Now().Add(acceptTimeout))
	conn, err := f.ln.Accept()
	if err != nil {
		t.Fatalf("server did not connect to the fake upstream within %v: %v", acceptTimeout, err)
	}

	p := &Peer{conn: conn, r: bufio.NewReader(conn)}
	t.Cleanup(func() { conn.Close() })

	for _, l := range append([]string{"# IS_Fake 1.00"}, f.AfterBanner...) {
		if err := p.SendLine(l); err != nil {
			t.Fatalf("failed to send the server banner to the upstream connection: %v", err)
		}
	}

	login, err := p.ReadPacket(time.Second)
	if err != nil {
		t.Fatalf("did not receive a login from the upstream connection: %v", err)
	}
	m := loginRe.FindStringSubmatch(login)
	if m == nil {
		t.Fatalf("login command not understood: %q", login)
	}
	p.User = m[1]

	verified := "unverified"
	if m[2] == fmt.Sprint(fap.AprsPasscode(m[1])) {
		verified = "verified"
	}
	for _, l := range f.BeforeLogresp {
		if err := p.SendLine(l); err != nil {
			t.Fatalf("failed to send a comment to the upstream connection: %v", err)
		}
	}
	if err := p.SendLine(fmt.Sprintf("# logresp %s %s, server %s", p.User, verified, f.call)); err != nil {
		t.Fatalf("failed to send logresp to the upstream connection: %v", err)
	}

	return p
}
