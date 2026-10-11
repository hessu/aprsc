package functional

import (
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
