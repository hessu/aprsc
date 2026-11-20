package functional

import (
	"fmt"
	"net"
	"regexp"
	"strings"
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
	harness.Parallel(t)
	s := harness.Start(t, "basic")

	const login = "N5CAL-1"
	const rxLogin = "N5CAL-2"
	tx := isclient.Connect(t, harness.Addr(t, 55580), login)
	rx := isclient.Connect(t, harness.Addr(t, 55580), rxLogin, isclient.WithFilter("p/OH"))

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

	// the command is found after a partial match of it: "fi" + "filter"
	t.Run("filter command after a partial match", func(t *testing.T) {
		rx.FilterCommand(t, "#fifilter p/OH")
		isclient.RunSteps(t, tx, rx, []isclient.Step{
			drop("K dropped", "K2SRC>APRS,qAR,"+login+":>should drop5",
				"OH2SRC>APRS,qAR,"+login+":>should pass5"),
		})
		rx.FilterCommand(t, "#ffilter p/N/K")
		isclient.RunSteps(t, tx, rx, []isclient.Step{
			drop("OH dropped", "OH2SRC>APRS,qAR,"+login+":>should drop6",
				"K2SRC>APRS,qAR,"+login+":>should pass6"),
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

	// A callsign-set filter (b/, p/, and the like) keeps its callsigns in
	// an array of its own, which goes with the filter when the chain is
	// replaced. Replace such chains a number of times, with a second
	// segment of a type extending the first, and check that the last one
	// is the one in effect.
	t.Run("replacing callsign-set filters", func(t *testing.T) {
		for i := 0; i < 20; i++ {
			rx.SetFilter(t, "b/OH1SRC/OH2SRC b/OH3SRC p/K/W")
		}
		rx.SetFilter(t, "p/N b/OH2SRC b/OH4SRC")
		isclient.RunSteps(t, tx, rx, []isclient.Step{
			drop("old buddy dropped", "OH1SRC>APRS,qAR,"+login+":>should drop9",
				"OH2SRC>APRS,qAR,"+login+":>should pass9"),
			drop("old prefix dropped", "K2SRC>APRS,qAR,"+login+":>should drop10",
				"OH4SRC>APRS,qAR,"+login+":>should pass10"),
			passSame("new prefix passes", "N2SRC>APRS,qAR,"+login+":>should pass11"),
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

// Text messages to SERVER are parsed by parse_aprs_message(). The message
// body length used to be 11 bytes too long, which broke commands that
// depend on an exact length ("filter default") whenever the message had no
// message id to cut the body short.
func TestServerMessageParsing(t *testing.T) {
	harness.Parallel(t)
	s := harness.Start(t, "basic")

	const login = "N5CAL-2"
	rx := isclient.Connect(t, harness.Addr(t, 55580), login, isclient.WithFilter("p/OH"))

	ackRe := func(id string) *regexp.Regexp {
		return regexp.MustCompile(`^SERVER>[^,]+,TCPIP\*,qAZ,TESTING::N5CAL-2  :ack` + id + `$`)
	}
	replyRe := func(text string) *regexp.Regexp {
		return regexp.MustCompile(`^SERVER>[^,]+,TCPIP\*,qAZ,TESTING::N5CAL-2  :` + text + `\{.*$`)
	}

	// message is the text sent after "SERVER   :", id is the message id
	// (empty for none), and reply is a regexp for the reply.
	cases := []struct {
		name, message, id, reply string
	}{
		{"filter with id", "filter p/OZ", "1", `filter p/OZ active`},
		{"filter without id", "filter p/OZ/ZZ", "", `filter p/OZ/ZZ active`},
		{"filter default without id", "filter default", "", `User filters reset to default`},
		{"filter default with id", "filter default", "2", `User filters reset to default`},
		{"unknown command without id", "foobar", "", `Unknown command`},
		{"message id only", "", "3", `Unknown command`},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			pkt := fmt.Sprintf("%s>APRS::%-9.9s:%s", login, "SERVER", tc.message)
			if tc.id != "" {
				pkt += "{" + tc.id
			}
			rx.Send(t, pkt)

			if tc.id != "" {
				ack, err := rx.ReadPacket(2 * time.Second)
				if err != nil {
					t.Fatalf("no ack: %v", err)
				}
				if !ackRe(tc.id).MatchString(ack) {
					t.Errorf("unexpected ack %q", ack)
				}
			}

			reply, err := rx.ReadPacket(2 * time.Second)
			if err != nil {
				t.Fatalf("no reply: %v", err)
			}
			if !replyRe(tc.reply).MatchString(reply) {
				t.Errorf("unexpected reply %q, wanted %q", reply, tc.reply)
			}
		})
	}

	// A message to SERVER inside a 3rd-party packet is not from the local
	// client, so it is not a command, and it is dropped. The outer header
	// is crafted so that the outer packet looks like a message too, which
	// used to get it acked and replied to.
	t.Run("3rd-party message is dropped", func(t *testing.T) {
		full := isclient.Connect(t, harness.Addr(t, 55152), "N5CAL-3")
		rx.Send(t, login+">APRS:}AB>C,D,E::SERVER   :filter p/OZ{9")

		// The next packet on the full feed must be the helper sent
		// after the 3rd-party packet.
		rx.Send(t, login+">APRS:>should pass")
		fwdRe := regexp.MustCompile(`^N5CAL-2>APRS,TCPIP\*,qAC,TESTING:>should pass$`)
		fwd, err := full.ReadPacket(2 * time.Second)
		if err != nil {
			t.Fatalf("helper packet not passed: %v", err)
		}
		if !fwdRe.MatchString(fwd) {
			t.Errorf("unexpected packet %q, wanted the helper", fwd)
		}

		// The next thing the client gets must be the ack to a real
		// command sent after the 3rd-party packet.
		rx.Send(t, fmt.Sprintf("%s>APRS::%-9.9s:filter?{10", login, "SERVER"))
		ack, err := rx.ReadPacket(2 * time.Second)
		if err != nil {
			t.Fatalf("no ack: %v", err)
		}
		if !ackRe("10").MatchString(ack) {
			t.Errorf("unexpected packet %q, wanted ack10", ack)
		}
	})

	s.Stop()
}

// A client which sends messages to SERVER but never reads the replies gets
// disconnected when a reply no longer fits in its output buffer. The reply
// is written while the client's input is being processed, and the server
// must stop processing it right there: the client is gone.
func TestServerMessageOutputFull(t *testing.T) {
	harness.Parallel(t)
	harness.RequireAprsc(t)
	s := harness.Start(t, "basic")

	const login = "N5CAL-9"
	conn, err := net.DialTimeout("tcp", harness.Addr(t, 55580), 5*time.Second)
	if err != nil {
		t.Fatalf("failed to connect: %v", err)
	}
	defer conn.Close()
	// a small receive buffer, so that the server's output backs up soon
	conn.(*net.TCPConn).SetReadBuffer(2048)

	// several lines in a write, so that there are more to process in the
	// server's input buffer after the one which gets the client closed
	var chunk strings.Builder
	for i := 0; i < 100; i++ {
		fmt.Fprintf(&chunk, "%s>APRS,TCPIP*::SERVER   :filter?{%d\r\n", login, i)
	}

	conn.SetWriteDeadline(time.Now().Add(30 * time.Second))
	if _, err := conn.Write([]byte("user " + login + " pass -1 vers aprsc-gotest 1.0\r\n")); err != nil {
		t.Fatalf("failed to send the login: %v", err)
	}

	closed := false
	for sent := 0; sent < 500<<20; sent += chunk.Len() {
		if _, err := conn.Write([]byte(chunk.String())); err != nil {
			if isclient.IsTimeout(err) {
				t.Fatalf("the server stopped reading after %d bytes without closing the connection", sent)
			}
			closed = true
			break
		}
	}
	if !closed {
		t.Fatal("the server did not close a client which does not read its replies")
	}

	// the server goes on serving the others
	tx := isclient.Connect(t, harness.Addr(t, 55580), "N5CAL-1")
	rx := isclient.Connect(t, harness.Addr(t, 55152), "N5CAL-2")
	isclient.TxRx(t, tx, rx,
		"SRC>APRS,qAR,N5CAL-1:>after a full output buffer",
		"SRC>APRS,qAR,N5CAL-1:>after a full output buffer")

	s.Stop()
}
