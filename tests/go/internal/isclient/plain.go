// Package isclient has plaintext APRS-IS test clients and the assertions
// built on them.
package isclient

import (
	"errors"
	"fmt"
	"math/rand"
	"net"
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

// Sender is something lines can be sent on: a Plain client, or a Peer which
// plays an upstream server.
type Sender interface {
	SendLine(line string) error
}

// Receiver is something packets can be received from. ReadPacket skips
// comment lines.
type Receiver interface {
	ReadPacket(timeout time.Duration) (string, error)
}

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
	vers     string
	udp      int
	appName  string // IS2 login
	dupes    bool   // IS2 dupe feed
}

// passcodeFor returns the correct passcode for a callsign.
func passcodeFor(callsign string) int16 {
	return fap.AprsPasscode(callsign)
}

// withUDPPort asks the server to send packets over UDP to the given port.
func withUDPPort(port int) Option { return func(c *config) { c.udp = port } }

// Option customizes Connect.
type Option func(*config)

// WithPasscode sets the passcode to log in with. The default is the correct
// passcode for the callsign.
func WithPasscode(p string) Option { return func(c *config) { c.passcode = p } }

// WithFilter sets the filter in the login line.
func WithFilter(f string) Option { return func(c *config) { c.filter = f } }

// WithVers sets everything after "vers" in the login line, for example
// "HR-IXPWINDi-123" to look like a client with known quirks.
func WithVers(v string) Option { return func(c *config) { c.vers = v } }

// Connect connects to the server at addr, retrying for a few seconds while the
// port is not open, and logs in as callsign. The test fails if it cannot. The
// connection is closed when the test finishes.
func Connect(t *testing.T, addr, callsign string, opts ...Option) *Plain {
	t.Helper()

	cfg := config{passcode: fmt.Sprint(fap.AprsPasscode(callsign))}
	for _, o := range opts {
		o(&cfg)
	}

	o := fap.ConnOptions{
		Callsign: callsign,
		Passcode: cfg.passcode,
		AppName:  "aprsc-gotest",
		AppVer:   "1.0",
		Filter:   cfg.filter,
		NoLogin:  true,
	}
	if cfg.vers != "" || cfg.udp != 0 {
		vers := cfg.vers
		if vers == "" {
			vers = "aprsc-gotest 1.0"
		}
		o.LoginLine = fmt.Sprintf("user %s pass %s vers %s", callsign, cfg.passcode, vers)
		if cfg.udp != 0 {
			o.LoginLine += fmt.Sprintf(" UDP %d", cfg.udp)
		}
		if cfg.filter != "" {
			o.LoginLine += " filter " + cfg.filter
		}
	}

	var conn *fap.Conn
	var err error
	deadline := time.Now().Add(connectRetry)
	for {
		conn, err = fap.Connect(addr, o)
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

// Send sends a line to the server, failing the test on error. It must be
// called from the goroutine of the test passed to Connect.
func (p *Plain) Send(line string) {
	p.t.Helper()
	if err := p.SendLine(line); err != nil {
		p.t.Fatalf("%s: failed to send %q: %v", p.Callsign, line, err)
	}
}

var filterActiveRe = regexp.MustCompile(`^# filter .* active$`)

// FilterAck tells whether the server under test replies to "#filter"
// commands. aprsc does; javAPRSSrvr does not, and with it FilterCommand just
// waits for FilterSettle instead.
var FilterAck = true

// FilterSettle is how long FilterCommand waits for a server which does not
// reply to filter commands.
const FilterSettle = 300 * time.Millisecond

// FilterCommand sends a "#filter" command line and waits for the server's
// "# filter ... active" reply, so that the filter is known to be in effect when
// it returns. The reply is a comment line, which ReadPacket skips, so it is
// read here with ReadLine. Other comment lines are ignored; a packet arriving
// first fails the test.
func (p *Plain) FilterCommand(line string) {
	p.t.Helper()
	p.Send(line)

	if !FilterAck {
		time.Sleep(FilterSettle)
		return
	}

	for {
		l, err := p.ReadLine(DefaultTimeout)
		if err != nil {
			p.t.Fatalf("%s: no filter reply after %q: %v", p.Callsign, line, err)
		}
		if filterActiveRe.MatchString(l) {
			return
		}
		if !strings.HasPrefix(l, "#") {
			p.t.Fatalf("%s: received packet %q while waiting for the filter reply to %q", p.Callsign, l, line)
		}
	}
}

// SetFilter sets the filter with a "#filter" command and waits for the reply.
func (p *Plain) SetFilter(filter string) {
	p.t.Helper()
	p.FilterCommand("#filter " + filter)
}

// isTimeout reports whether err is a network timeout.
func isTimeout(err error) bool {
	var ne net.Error
	return errors.As(err, &ne) && ne.Timeout()
}

// send sends a line, failing the test on error.
func send(t *testing.T, tx Sender, line string) {
	t.Helper()
	if err := tx.SendLine(line); err != nil {
		t.Fatalf("failed to send %q: %v", line, err)
	}
}

// TxRx sends a line on tx and expects to receive exactly want on rx.
func TxRx(t *testing.T, tx Sender, rx Receiver, sent, want string) {
	t.Helper()

	send(t, tx, sent)

	got, err := rx.ReadPacket(DefaultTimeout)
	if err != nil {
		t.Errorf("did not receive %q (sent %q): %v", want, sent, err)
		return
	}
	if got != want {
		t.Errorf("sent %q:\n got  %q\n want %q", sent, got, want)
	}
}

// ExpectDrop sends the packet drop on tx and then a helper packet, and expects
// that rx receives only the helper. A random token is appended to both
// packets, so that neither is a duplicate of anything the server has seen and
// the drop must be for the reason being tested.
func ExpectDrop(t *testing.T, tx Sender, rx Receiver, drop, helper string) {
	t.Helper()
	expectDrop(t, tx, rx, drop, helper, dropOpts{})
}

// ExpectDropExact is like ExpectDrop, but sends the packet to be dropped
// exactly as given, for tests of duplicate detection. The helper still gets a
// random token.
func ExpectDropExact(t *testing.T, tx Sender, rx Receiver, drop, helper string) {
	t.Helper()
	expectDrop(t, tx, rx, drop, helper, dropOpts{exact: true})
}

// ExpectDropLiteral is like ExpectDrop, but sends both packets exactly as
// given. It is for packets which would no longer parse with a token appended,
// such as NMEA sentences with a checksum. The helper is recognized by its
// content, ignoring the digipeater path.
func ExpectDropLiteral(t *testing.T, tx Sender, rx Receiver, drop, helper string) {
	t.Helper()
	expectDrop(t, tx, rx, drop, helper, dropOpts{exact: true, literalHelper: true})
}

type dropOpts struct {
	exact         bool // send the dropped packet without a token
	literalHelper bool // send the helper without a token
}

func expectDrop(t *testing.T, tx Sender, rx Receiver, drop, helper string, o dropOpts) {
	t.Helper()

	dropKey := ""
	if !o.exact {
		dropKey = fmt.Sprintf("drop.%d", rand.Intn(1000000))
		drop += " " + dropKey
	}
	helperKey := ""
	helperSent := helper
	if !o.literalHelper {
		helperKey = fmt.Sprintf("helper.%d", rand.Intn(1000000))
		helperSent += " " + helperKey
	}

	send(t, tx, drop)
	send(t, tx, helperSent)

	got, err := rx.ReadPacket(DefaultTimeout)
	if err != nil {
		t.Errorf("did not receive the helper packet %q after %q: %v", helperSent, drop, err)
		return
	}

	if o.literalHelper {
		if stripPath(got) == stripPath(helper) {
			return
		}
	} else if strings.Contains(got, helperKey) {
		return
	}

	if (!o.exact && strings.Contains(got, dropKey)) || (o.exact && stripPath(got) == stripPath(drop)) {
		t.Errorf("server forwarded a packet it should have dropped: %q", got)
	} else {
		t.Errorf("server sent an unexpected packet %q instead of the helper %q", got, helperSent)
	}

	// resynchronize: consume the helper that must still be coming
	rx.ReadPacket(DefaultTimeout)
}

// ExpectSilence fails the test if rx receives a packet within d.
func ExpectSilence(t *testing.T, rx Receiver, d time.Duration, what string) {
	t.Helper()

	got, err := rx.ReadPacket(d)
	switch {
	case err == nil:
		t.Errorf("%s: received unexpected data %q", what, got)
	case !isTimeout(err):
		t.Errorf("%s: connection error while expecting silence: %v", what, err)
	}
}

// Step is one check in a sequence run by RunSteps.
type Step struct {
	Name string
	Send string
	// Want is the packet expected on the receiving side. If it is empty, the
	// packet must be dropped instead, and Helper is the packet used to prove
	// that the server is still forwarding.
	Want   string
	Helper string
	// Exact sends the dropped packet without a unique token, so that it can
	// be a duplicate of an earlier packet.
	Exact bool
	// Literal sends both the dropped packet and the helper without tokens.
	Literal bool
	// Before is an extra line sent right before Send. If it is expected to
	// be dropped, the step fails when it shows up instead of the helper.
	Before string
}

// RunSteps runs the steps in order as subtests. Steps depend on the server
// state left by earlier ones, so they are not run in parallel.
func RunSteps(t *testing.T, tx Sender, rx Receiver, steps []Step) {
	t.Helper()

	for _, st := range steps {
		name := st.Name
		if name == "" {
			name = st.Send
		}
		t.Run(name, func(t *testing.T) {
			if st.Before != "" {
				send(t, tx, st.Before)
			}

			switch {
			case st.Want != "":
				TxRx(t, tx, rx, st.Send, st.Want)
			case st.Literal:
				ExpectDropLiteral(t, tx, rx, st.Send, st.Helper)
			case st.Exact:
				ExpectDropExact(t, tx, rx, st.Send, st.Helper)
			default:
				ExpectDrop(t, tx, rx, st.Send, st.Helper)
			}
		})
	}
}

var pathRe = regexp.MustCompile(`^([^>]+)>([^,:]+)(?:,[^:]*)?(:.*)$`)

// stripPath removes the digipeater path from a TNC2 line, for comparing
// packets whose paths the server may have changed.
func stripPath(line string) string {
	return pathRe.ReplaceAllString(line, "$1>$2$3")
}
