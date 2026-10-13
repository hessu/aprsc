package functional

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// The in-band upgrade of plaintext client connections to IS2: "#is2 hello v=1"
// from the client before its login line, and the server's "#is2 hello v=1"
// answer, after which the connection continues in IS2. The server does not
// announce the upgrade after its banner. See doc/IS2UPGRADE.md.

const upgradeLogin = "user N0TEST pass -1 vers gotest 1.0"

// is2BasicWith returns the is2-basic configuration with changes: lines
// appended, and listener lines edited.
func is2BasicWith(t *testing.T, edit func(line string) string, extra ...string) string {
	t.Helper()

	src, err := os.ReadFile(filepath.Join(harness.TestsDir(), "cfg-aprsc", "is2-basic"))
	if err != nil {
		t.Fatal(err)
	}
	var out []string
	for _, l := range strings.Split(string(src), "\n") {
		if edit != nil {
			l = edit(l)
		}
		out = append(out, l)
	}
	return strings.Join(append(out, extra...), "\n") + "\n"
}

// checkNoUpgrade checks that the lines a server sent before its login reply
// do not offer or do an upgrade.
func checkNoUpgrade(t *testing.T, what string, lines []string) {
	t.Helper()

	if len(lines) == 0 || !strings.HasPrefix(lines[len(lines)-1], "# logresp") {
		t.Errorf("%s: the login was not answered with a logresp: %q", what, lines)
	}
	for _, l := range lines {
		if strings.HasPrefix(l, "#is2") {
			t.Errorf("%s: the server sent %q (all: %q)", what, l, lines)
		}
	}
}

func TestIS2Upgrade(t *testing.T) {
	harness.Parallel(t)
	harness.RequireIS2(t)
	s := harness.Start(t, "is2-basic")

	// a plaintext client sees only the banner before its login reply: the
	// upgrade is not announced, as other servers uplinking to this one
	// expect the logresp right after the banner
	lines := isclient.PreLogin(t, harness.Addr(t, 55580), upgradeLogin)
	if len(lines) != 2 || !strings.HasPrefix(lines[0], "# aprsc") || !strings.HasPrefix(lines[1], "# logresp") {
		t.Errorf("plaintext login: got %q, want the banner and a logresp", lines)
	}

	// hellos which do not upgrade: the client logs in on plaintext
	for _, hello := range []string{"#is2 hello v=2,3", "#is2 hello", "#is2 hello v=x", "#is2 helloworld v=1"} {
		lines := isclient.PreLogin(t, harness.Addr(t, 55580), hello, upgradeLogin)
		if len(lines) == 0 || !strings.HasPrefix(lines[len(lines)-1], "# logresp") {
			t.Errorf("%q: the login was not answered with a logresp: %q", hello, lines)
		}
	}

	// upgrades on an igate port and a full feed port
	igate := isclient.ConnectIS2Upgrade(t, harness.Addr(t, 55580), "N5CAL-2")
	full := isclient.ConnectIS2Upgrade(t, harness.Addr(t, 55152), "N5CAL-3")
	for _, c := range []*isclient.IS2{igate, full} {
		p := c.Preamble
		if len(p) != 2 || !strings.HasPrefix(p[0], "# aprsc") || p[1] != "#is2 hello v=1" {
			t.Errorf("%s: got %q before the switch, want the banner and \"#is2 hello v=1\"", c.Callsign, p)
		}
		if err := c.Ping(); err != nil {
			t.Errorf("%s: ping after the upgrade failed: %v", c.Callsign, err)
		}
	}

	plainFull := isclient.Connect(t, harness.Addr(t, 55152), "N5CAL-1")
	plainIgate := isclient.Connect(t, harness.Addr(t, 55580), "N5CAL-4")

	// a hello after the login is only a comment
	plainIgate.Send(t, "#is2 hello v=1")

	// packets both ways between the upgraded and plaintext clients; the
	// full feeds get all of them
	const toIS2 = "SRC>DST,qAR,IGATE:plaintext to upgraded"
	isclient.TxRx(t, plainIgate, full, toIS2, toIS2)
	if got, err := plainFull.ReadPacket(isclient.DefaultTimeout); err != nil || got != toIS2 {
		t.Errorf("plaintext full feed: got %q (%v), want %q", got, err, toIS2)
	}
	const fromIS2 = "SRC>DST,qAR,IGATE:upgraded to plaintext"
	isclient.TxRx(t, igate, plainFull, fromIS2, fromIS2)

	// the status tells which clients upgraded, and which ports offer it
	j := getStatus(t)
	links := j.clientLinks()
	for name, want := range map[string]string{"N5CAL-1": "", "N5CAL-2": "is2", "N5CAL-3": "is2", "N5CAL-4": ""} {
		if links[name] != want {
			t.Errorf("status of client %s: link %q, want %q", name, links[name], want)
		}
	}
	for _, l := range j.Listeners {
		name, _ := l["name"].(string)
		want := !strings.HasPrefix(name, "IS2 ")
		if up, ok := l["is2_upgrade"].(bool); !ok || up != want {
			t.Errorf("listener %q is2_upgrade is %v, want %v", name, l["is2_upgrade"], want)
		}
	}

	s.Stop()
}

// The text arriving in pieces, with the line terminators in separate reads,
// plaintext lines after the switch, and the STX of the first frame alone.
func TestIS2UpgradeSlow(t *testing.T) {
	harness.Parallel(t)
	harness.RequireIS2(t)
	s := harness.Start(t, "is2-basic")

	c := isclient.ConnectIS2Upgrade(t, harness.Addr(t, 55580), "N5CAL-2", isclient.WithSlowUpgrade())
	if err := c.Ping(); err != nil {
		t.Errorf("ping after the upgrade failed: %v", err)
	}
	// the login line and the two comments, not the empty line
	if n := s.LogCount("IS2 upgrade: discarding a plaintext line"); n != 3 {
		t.Errorf("the server discarded %d plaintext lines after the switch, want 3", n)
	}

	plainFull := isclient.Connect(t, harness.Addr(t, 55152), "N5CAL-1")
	const fromIS2 = "SRC>DST,qAR,IGATE:slowly upgraded to plaintext"
	isclient.TxRx(t, c, plainFull, fromIS2, fromIS2)

	s.Stop()
}

// The upgrade is not offered on servers with picky clients (quirks mode), on
// servers pretending to be other software (fake_version), or on ports where
// it is disabled.
func TestIS2UpgradeNotOffered(t *testing.T) {
	harness.Parallel(t)
	harness.RequireIS2(t)

	for _, tc := range []struct {
		name   string
		config string
		port   int  // the port which does not offer the upgrade
		other  bool // the other plaintext port still offers it
	}{
		{"quirks_mode", is2BasicWith(t, nil, "quirks_mode yes"), 55580, false},
		{"fake_version", is2BasicWith(t, nil, "fake_version otherserver-1.0"), 55580, false},
		{"noIS2upgrade", is2BasicWith(t, func(l string) string {
			if strings.HasPrefix(l, `Listen "Igate port"`) {
				return l + " noIS2upgrade"
			}
			return l
		}), 55580, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			s := harness.StartText(t, "is2-upgrade-"+tc.name, tc.config)

			checkNoUpgrade(t, "plaintext login", isclient.PreLogin(t, harness.Addr(t, tc.port), upgradeLogin))
			checkNoUpgrade(t, "hello", isclient.PreLogin(t, harness.Addr(t, tc.port), isclient.UpgradeHello, upgradeLogin))
			if _, err := isclient.DialIS2Upgrade(harness.Addr(t, tc.port), "N5CAL-2"); !errors.Is(err, isclient.ErrNoUpgrade) {
				t.Errorf("upgrade: got error %v, want %v", err, isclient.ErrNoUpgrade)
			}

			// the full feed port offers it, unless the whole server does not
			lines := isclient.PreLogin(t, harness.Addr(t, 55152), isclient.UpgradeHello, upgradeLogin)
			offered := len(lines) > 0 && lines[len(lines)-1] == "#is2 hello v=1"
			if offered != tc.other {
				t.Errorf("the other port offered the upgrade: %v, want %v (%q)", offered, tc.other, lines)
			}

			for _, l := range getStatus(t).Listeners {
				name, _ := l["name"].(string)
				want := tc.other && name == "Full feed with CWOP"
				if up, ok := l["is2_upgrade"].(bool); !ok || up != want {
					t.Errorf("listener %q is2_upgrade is %v, want %v", name, l["is2_upgrade"], want)
				}
			}

			s.Stop()
		})
	}
}

// A server built without IS2 (the default) neither offers the upgrade nor
// answers a hello, and the client logs in on plaintext, as on any other
// server software.
func TestIS2UpgradeWithoutIS2(t *testing.T) {
	harness.Parallel(t)
	harness.RequireAprsc(t)
	harness.RequireNoIS2(t)
	s := harness.Start(t, "basic")

	checkNoUpgrade(t, "plaintext login", isclient.PreLogin(t, harness.Addr(t, 55580), upgradeLogin))
	checkNoUpgrade(t, "hello", isclient.PreLogin(t, harness.Addr(t, 55580), isclient.UpgradeHello, upgradeLogin))

	for _, l := range getStatus(t).Listeners {
		if up, ok := l["is2_upgrade"].(bool); !ok || up {
			t.Errorf("listener %q is2_upgrade is %v, want false", l["name"], l["is2_upgrade"])
		}
	}

	s.Stop()
}
