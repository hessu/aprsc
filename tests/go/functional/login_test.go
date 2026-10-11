package functional

import (
	"fmt"
	"strings"
	"testing"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// Log in to the full feed port with a valid passcode. Port of t/01login.t.
func TestLogin(t *testing.T) {
	s := harness.Start(t, "basic")

	c := isclient.Connect(t, "localhost:55152", "OH7LZB")

	if !c.LogResp.Verified {
		t.Errorf("login was not verified: %q", c.LogResp.Raw)
	}
	if c.LogResp.Callsign != "OH7LZB" || c.LogResp.Server != "TESTING" {
		t.Errorf("unexpected logresp: %q", c.LogResp.Raw)
	}
	if harness.IsAprsc() && !strings.HasPrefix(c.Banner(), "# aprsc ") {
		t.Errorf("unexpected banner: %q", c.Banner())
	}

	if err := c.Close(); err != nil {
		t.Errorf("failed to disconnect: %v", err)
	}

	s.Stop()
}

// Logins which must be rejected: usernames on the built-in list and those
// matching the DisallowLoginCall patterns of the configuration, and login
// lines with no version number after the software name. Port of
// t/02login-reject.t.
func TestLoginReject(t *testing.T) {
	s := harness.Start(t, "basic")

	// expectReject sends a login line and expects the server to refuse it,
	// with the reply want on aprsc.
	expectReject := func(t *testing.T, login, want string) {
		t.Helper()

		reply, accepted := isclient.LoginReply(t, "localhost:55152", login)
		if accepted {
			t.Errorf("login %q was accepted: %q", login, reply)
			return
		}
		if harness.IsAprsc() && reply != want {
			t.Errorf("login %q was refused with %q, want %q", login, reply, want)
		}
	}

	// "pass" is on the built-in list. The others match the configured
	// patterns "LOGINA LOGINB *prrej mi*rej sufre*", regardless of case.
	for _, call := range []string{"pass", "LOGINA", "loginb", "0prrej", "mi0rej", "sufrej"} {
		t.Run(call, func(t *testing.T) {
			expectReject(t,
				fmt.Sprintf("user %s pass %s vers aprsc-gotest 1.0", call, isclient.Passcode(call)),
				"# Login by user not allowed")
		})
	}

	// A client which sends "vers AppName filter ..." without a version number
	// between the software name and the filter keyword, and one which sends
	// nothing at all after the software name.
	const noVersion = "# Invalid login: software name and version are not separated by a space"
	t.Run("filter as version number", func(t *testing.T) {
		expectReject(t, "user OH7LZB pass -1 vers MyApp filter r/0/0/0", noVersion)
	})
	t.Run("no version number", func(t *testing.T) {
		expectReject(t, "user OH7LZB pass -1 vers MyApp", noVersion)
	})

	s.Stop()
}
