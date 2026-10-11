package functional

import (
	"fmt"
	"regexp"
	"testing"
	"time"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// Filter command testing.
//
// A filter can be set by a "server adjunct command" in the login:
// user <username> pass <passcode> vers <soft> <vers> filter <filterstring>
//
// or with a #command (case insensitive) in the incoming stream:
// "# filter <filterstring>", "#filter <filterstring>",
// "#alfhilawuehflwieuhffilter <filterstring>", "#FILTER <filterstring>"
//
// or with an APRS message with a recipient call of "SERVER", saying
// "filter <filterstring>". Yes, the command can be prefixed with anything,
// even in the "user" command.
//
// Port of t/30filter-cmd.t.
func TestFilterCommand(t *testing.T) {
	s := harness.Start(t, "basic")

	const login = "N5CAL-1"
	const rxLogin = "N5CAL-2"
	tx := isclient.Connect(t, "localhost:55580", login)
	rx := isclient.Connect(t, "localhost:55580", rxLogin, isclient.WithFilter("p/OH"))

	// the initial p/ filter from the login line works
	isclient.RunSteps(t, tx, rx, []isclient.Step{
		passSame("login filter", "OH2SRC>APRS,qAR,"+login+":>should pass"),
	})

	t.Run("filter command", func(t *testing.T) {
		rx.SetFilter(t, "p/N/K")
		isclient.RunSteps(t, tx, rx, []isclient.Step{
			drop("OH dropped", "OH2SRC>APRS,qAR,"+login+":>should drop",
				"K2SRC>APRS,qAR,"+login+":>should pass"),
			drop("OH dropped, N passes", "OH2SRC>APRS,qAR,"+login+":>should drop2",
				"N2SRC>APRS,qAR,"+login+":>should pass2"),
		})
	})

	t.Run("filter command with spaces", func(t *testing.T) {
		rx.FilterCommand(t, "#  fIlTer   p/N/K  ")
		isclient.RunSteps(t, tx, rx, []isclient.Step{
			drop("OH dropped", "OH2SRC>APRS,qAR,"+login+":>should drop3",
				"K2SRC>APRS,qAR,"+login+":>should pass3"),
			drop("OH dropped, N passes", "OH2SRC>APRS,qAR,"+login+":>should drop4",
				"N2SRC>APRS,qAR,"+login+":>should pass4"),
		})
	})

	t.Run("filter command with rubbish in front", func(t *testing.T) {
		rx.FilterCommand(t, "#blaablaaanxauyg3iuyq2gFILTER p/OG/OF/OH")
		isclient.RunSteps(t, tx, rx, []isclient.Step{
			drop("K dropped", "K2SRC>APRS,qAR,"+login+":>should drop5",
				"OH2SRC>APRS,qAR,"+login+":>should pass5"),
			drop("N dropped", "N2SRC>APRS,qAR,"+login+":>should drop6",
				"OH2SRC>APRS,qAR,"+login+":>should pass6"),
		})
	})

	// The server acknowledges a message to SERVER, and replies to it.
	ackRe := func(id string) *regexp.Regexp {
		return regexp.MustCompile(`^SERVER>[^,]+,TCPIP\*,qAZ,TESTING::N5CAL-2  :ack` + id + `$`)
	}
	replyRe := regexp.MustCompile(`^SERVER>[^,]+,TCPIP\*,qAZ,TESTING::N5CAL-2  :filter.*active\{.*$`)

	serverMessage := func(t *testing.T, text, id string) {
		t.Helper()
		rx.Send(t, fmt.Sprintf("%s>APRS::%-9.9s:%s{%s", rxLogin, "SERVER", text, id))

		ack, err := rx.ReadPacket(2 * time.Second)
		if err != nil {
			t.Fatalf("no ack for message %q: %v", text, err)
		}
		if !ackRe(id).MatchString(ack) {
			t.Errorf("unexpected ack %q", ack)
		}

		reply, err := rx.ReadPacket(2 * time.Second)
		if err != nil {
			t.Fatalf("no reply to message %q: %v", text, err)
		}
		if !replyRe.MatchString(reply) {
			t.Errorf("unexpected reply %q", reply)
		}
	}

	t.Run("filter by message", func(t *testing.T) {
		serverMessage(t, "filter p/OZ/ZZ", "ax")

		isclient.RunSteps(t, tx, rx, []isclient.Step{
			drop("OH dropped, OZ passes", "OH2SRC>APRS,qAR,"+login+":>should drop7",
				"OZ2SRC>APRS,qAR,"+login+":>should pass7"),
			drop("OH dropped, ZZ passes", "OH2SRC>APRS,qAR,"+login+":>should drop8",
				"ZZ2SRC>APRS,qAR,"+login+":>should pass8"),
		})
	})

	t.Run("filter query by message", func(t *testing.T) {
		serverMessage(t, "filter p/OZ/ZZ", "bx")
	})

	s.Stop()
}
