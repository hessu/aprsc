package functional

import (
	"testing"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// Some bad packets which should be allowed in quirks mode, for clients with
// known bad-and-unmaintained software. Port of t/12quirks-mode.t.
func TestQuirksMode(t *testing.T) {
	s := harness.Start(t, "basic")

	const login = "N5CAL-1"
	const vers = "HR-IXPWINDi-123"
	tx := isclient.Connect(t, "localhost:55580", login, isclient.WithVers(vers))
	rx := isclient.Connect(t, "localhost:55152", "N5CAL-2", isclient.WithVers(vers))

	steps := []isclient.Step{
		pass("spaces in front of srccall",
			"   SRC>DST,qAR,"+login+":>spaces in beginning",
			"SRC>DST,qAR,"+login+":>spaces in beginning"),
		pass("nulls in front of srccall",
			"\x00SRC>DST,qAR,"+login+":>nulls in beginning",
			"SRC>DST,qAR,"+login+":>nulls in beginning"),
		pass("spaces at end of srccall",
			"SRC  >DST,qAR,"+login+":>spaces in end of srccall",
			"SRC>DST,qAR,"+login+":>spaces in end of srccall"),
		pass("spaces in path callsign",
			"SRC>DST,qAR,"+login+" :>spaces in end of path element",
			"SRC>DST,qAR,"+login+":>spaces in end of path element"),
		drop("bad srccall dropped without quirks",
			"default setting is>DST,qAR,FOO:bad srccall",
			"SRC>LONG:dummy"),
	}

	isclient.RunSteps(t, tx, rx, steps)

	s.Stop()
}
