// Package isclient has plaintext APRS-IS test clients and the assertions
// built on them.
package isclient

import (
	"fmt"
	"math/rand"
	"regexp"
	"strings"
	"testing"
	"time"

	fap "github.com/hessu/go-aprs-fap"
)

// DefaultTimeout is how long the helpers wait for a packet before deciding
// that the server is not going to send it.
const DefaultTimeout = 5 * time.Second

// connectRetry is how long Connect keeps trying while the listener is not
// accepting yet.
const connectRetry = 8 * time.Second

// Plain is a plaintext APRS-IS client connected to a test server.
type Plain struct {
	*fap.Conn
	t *testing.T

	Callsign string
	LogResp  *fap.LogResp
}

type config struct {
	passcode string
	filter   string
}

// Option customizes Connect.
type Option func(*config)

// WithPasscode sets the passcode to log in with. The default is the correct
// passcode for the callsign.
func WithPasscode(p string) Option { return func(c *config) { c.passcode = p } }

// WithFilter sets the filter in the login line.
func WithFilter(f string) Option { return func(c *config) { c.filter = f } }

// Connect connects to the server at addr, retrying for a few seconds while the
// port is not open, and logs in as callsign. The test fails if it cannot. The
// connection is closed when the test finishes.
func Connect(t *testing.T, addr, callsign string, opts ...Option) *Plain {
	t.Helper()

	cfg := config{passcode: fmt.Sprint(fap.AprsPasscode(callsign))}
	for _, o := range opts {
		o(&cfg)
	}

	var conn *fap.Conn
	var err error
	deadline := time.Now().Add(connectRetry)
	for {
		conn, err = fap.Connect(addr, fap.ConnOptions{
			Callsign: callsign,
			Passcode: cfg.passcode,
			AppName:  "aprsc-gotest",
			AppVer:   "1.0",
			Filter:   cfg.filter,
			NoLogin:  true,
		})
		if err == nil {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("failed to connect to %s: %v", addr, err)
		}
		time.Sleep(100 * time.Millisecond)
	}

	p := &Plain{Conn: conn, t: t, Callsign: callsign}
	t.Cleanup(func() { conn.Close() })

	p.LogResp, err = conn.Login()
	if err != nil {
		t.Fatalf("%s: login to %s failed: %v", callsign, addr, err)
	}

	return p
}

// Send sends a line to the server, failing the test on error.
func (p *Plain) Send(line string) {
	p.t.Helper()
	if err := p.SendLine(line); err != nil {
		p.t.Fatalf("%s: failed to send %q: %v", p.Callsign, line, err)
	}
}

// Recv returns the next non-comment line, or an error on timeout or if the
// connection is gone.
func (p *Plain) Recv() (string, error) {
	return p.ReadPacket(DefaultTimeout)
}

// TxRx sends a line on tx and expects to receive exactly want on rx.
func TxRx(t *testing.T, tx, rx *Plain, sent, want string) {
	t.Helper()

	tx.Send(sent)

	got, err := rx.Recv()
	if err != nil {
		t.Errorf("did not receive %q (sent %q): %v", want, sent, err)
		return
	}
	if got != want {
		t.Errorf("sent %q:\n got  %q\n want %q", sent, got, want)
	}
}

// ExpectDrop sends the packet drop on tx and then a helper packet, and expects
// that rx receives only the helper. The helper is the given packet with a
// random token appended, so that it is never a duplicate of anything.
func ExpectDrop(t *testing.T, tx, rx *Plain, drop, helper string) {
	t.Helper()

	key := fmt.Sprintf("helper.%d", rand.Intn(1000000))
	helperKeyed := helper + " " + key

	tx.Send(drop)
	tx.Send(helperKeyed)

	got, err := rx.Recv()
	if err != nil {
		t.Errorf("did not receive the helper packet %q after %q: %v", helperKeyed, drop, err)
		return
	}
	if strings.Contains(got, key) {
		return
	}

	if stripPath(got) == stripPath(drop) {
		t.Errorf("server forwarded a packet it should have dropped: %q", got)
	} else {
		t.Errorf("server sent an unexpected packet %q instead of the helper %q", got, helperKeyed)
	}

	// resynchronize: consume the helper that must still be coming
	rx.Recv()
}

var pathRe = regexp.MustCompile(`^([^>]+)>([^,:]+)(?:,[^:]*)?(:.*)$`)

// stripPath removes the digipeater path from a TNC2 line, for comparing
// packets whose paths the server may have changed.
func stripPath(line string) string {
	return pathRe.ReplaceAllString(line, "$1>$2$3")
}
