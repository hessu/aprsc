package functional

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// configWith returns the text of the named configuration in cfg-aprsc with
// the extra lines appended, for a server started with harness.StartText.
func configWith(t *testing.T, name string, extra ...string) string {
	t.Helper()

	src, err := os.ReadFile(filepath.Join(harness.TestsDir(), "cfg-aprsc", name))
	if err != nil {
		t.Fatal(err)
	}
	return string(src) + strings.Join(extra, "\n") + "\n"
}

// pass is a step where the packet must come out of the receiver as want.
func pass(name, send, want string) isclient.Step {
	return isclient.Step{Name: name, Send: send, Want: want}
}

// passSame is a step where the packet must come out of the receiver unchanged.
func passSame(name, packet string) isclient.Step {
	return pass(name, packet, packet)
}

// drop is a step where the packet must be dropped; only the helper packet
// comes out. The packet is sent with a unique token appended, so that it is
// not dropped as a duplicate of an earlier packet.
func drop(name, send, helper string) isclient.Step {
	return isclient.Step{Name: name, Send: send, Helper: helper}
}

// dropDupe is like drop, but the packet is sent exactly as given, so that the
// server can drop it as a duplicate.
func dropDupe(name, send, helper string) isclient.Step {
	return isclient.Step{Name: name, Send: send, Helper: helper, Exact: true}
}

// dropLiteral is like drop, but neither the dropped packet nor the helper gets
// a token, for packets which must stay byte-for-byte intact to parse.
func dropLiteral(name, send, helper string) isclient.Step {
	return isclient.Step{Name: name, Send: send, Helper: helper, Literal: true}
}
