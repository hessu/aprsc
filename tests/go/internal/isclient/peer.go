package isclient

import (
	"bufio"
	"errors"
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

	p, err := f.accept(t)
	if err != nil {
		t.Fatal(err)
	}
	return p
}

// AcceptLive is Accept for a server which may connect and then drop the
// connection on its own, as the server does when its uplink thread is
// stopped while it is connecting. A connection the server closes before
// logging in, or within the quiet time after it, is skipped, and the first
// one which stays open is returned. Lines the server sends meanwhile, such
// as keepalives, are read and dropped.
func (f *FakeUpstream) AcceptLive(t *testing.T, quiet time.Duration) *Peer {
	t.Helper()

	deadline := time.Now().Add(acceptTimeout)
	for time.Now().Before(deadline) {
		p, err := f.accept(t)
		if err != nil {
			if p == nil {
				t.Fatal(err)
			}
			t.Logf("skipping a connection from the server: %v", err)
			p.Close()
			continue
		}
		// a connection which the server is closing says so within the
		// quiet time; one which stays open sends keepalives, or nothing
		for {
			_, err := p.ReadLine(quiet)
			if err == nil {
				continue
			}
			if isTimeout(err) {
				return p
			}
			t.Logf("skipping a connection from the server, closed after login: %v", err)
			p.Close()
			break
		}
	}
	t.Fatalf("server did not keep up a connection to the fake upstream within %v", acceptTimeout)
	return nil
}

// accept does the work of Accept. When it returns an error with a Peer, the
// server connected but the login did not complete.
func (f *FakeUpstream) accept(t *testing.T) (*Peer, error) {
	f.ln.SetDeadline(time.Now().Add(acceptTimeout))
	conn, err := f.ln.Accept()
	if err != nil {
		return nil, fmt.Errorf("server did not connect to the fake upstream within %v: %v", acceptTimeout, err)
	}

	p := &Peer{conn: conn, r: bufio.NewReader(conn)}
	t.Cleanup(func() { conn.Close() })

	for _, l := range append([]string{"# IS_Fake 1.00"}, f.AfterBanner...) {
		if err := p.SendLine(l); err != nil {
			return p, fmt.Errorf("failed to send the server banner to the upstream connection: %v", err)
		}
	}

	login, err := p.ReadPacket(time.Second)
	if err != nil {
		return p, fmt.Errorf("did not receive a login from the upstream connection: %v", err)
	}
	m := loginRe.FindStringSubmatch(login)
	if m == nil {
		return p, fmt.Errorf("login command not understood: %q", login)
	}
	p.User = m[1]

	verified := "unverified"
	if m[2] == fmt.Sprint(fap.AprsPasscode(m[1])) {
		verified = "verified"
	}
	for _, l := range f.BeforeLogresp {
		if err := p.SendLine(l); err != nil {
			return p, fmt.Errorf("failed to send a comment to the upstream connection: %v", err)
		}
	}
	if err := p.SendLine(fmt.Sprintf("# logresp %s %s, server %s", p.User, verified, f.call)); err != nil {
		return p, fmt.Errorf("failed to send logresp to the upstream connection: %v", err)
	}

	return p, nil
}

func isTimeout(err error) bool {
	var ne net.Error
	return errors.As(err, &ne) && ne.Timeout()
}
