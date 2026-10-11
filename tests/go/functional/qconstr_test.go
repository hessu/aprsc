package functional

import (
	"strings"
	"testing"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

const serverCall = "TESTING"

// qProto rewrites the q constructs in s, written for protocol A, to the given
// protocol letter (A or O).
func qProto(s, proto string) string {
	return strings.ReplaceAll(s, "qA", "q"+proto)
}

// qSteps builds steps from packet pairs written for protocol A, with q
// constructs rewritten to the protocol under test.
type qBuilder struct{ proto string }

func (b qBuilder) pass(name, send, want string) isclient.Step {
	return pass(name, qProto(send, b.proto), qProto(want, b.proto))
}

func (b qBuilder) drop(name, send string) isclient.Step {
	return drop(name, qProto(send, b.proto), "SRC>DST:dummy")
}

// qcClientOnlyPhase1 are the q construct cases for a verified client on a
// client-only port. proto is the q construct protocol letter the server is
// configured with, and wrong is a protocol letter it is not.
func qcClientOnlyPhase1(login, proto, wrong string) []isclient.Step {
	b := qBuilder{proto}

	return []isclient.Step{
		// Not in the Q algorithm, but: packets having srccall == login, and
		// having no Q construct, must have their digipeater path truncated
		// away and replaced with ,TCPIP* and then the Q construct.
		b.pass("tcpip-path-replace1",
			login+">DST:tcpip-path-replace1",
			login+">DST,TCPIP*,qAC,TESTING:tcpip-path-replace1"),
		b.pass("tcpip-path-replace2",
			login+">DST,DIGI1,DIGI5*:tcpip-path-replace2",
			login+">DST,TCPIP*,qAC,TESTING:tcpip-path-replace2"),
		b.pass("tcpip-path-replace3",
			login+">DST,TCPIP*:tcpip-path-replace3",
			login+">DST,TCPIP*,qAC,TESTING:tcpip-path-replace3"),
		b.pass("tcpip-path-replace4",
			login+">DST,qAR,"+login+":tcpip-path-replace4",
			login+">DST,TCPIP*,qAC,TESTING:tcpip-path-replace4"),
		b.pass("tcpip-path-replace5",
			login+">DST,WIDE2-2,qAR,"+login+":tcpip-path-replace5",
			login+">DST,TCPIP*,qAC,TESTING:tcpip-path-replace5"),

		// Not in the algorithm, but done in javap/javap4/aprsc: packets
		// having a different protocol than the configured one in the q
		// construct are dropped.
		drop("wrong q protocol id",
			"SRCCALL>DST,q"+wrong+"R,GATES:testing wrong Q protocol ID", "SRC>DST:dummy"),

		// All packets: if a q construct is last in the path (no call
		// following the qPT), delete the qPT. The test intentionally has a
		// qAR without a trailing call, and it is converted to a qAO.
		b.pass("q construct without a call",
			"SRC>DST,DIGI1,DIGI5*,qAR:a4ufy",
			"SRC>DST,DIGI1,DIGI5*,qAO,"+login+":a4ufy"),

		// Not in the algorithm, but: if a path element after the q construct
		// has a '*' or other crap in the callsign, the packet is dropped.
		b.drop("star after q construct",
			"SRCCALL>DST,DIGI1*,qAR,GATES*:testing * after Q construct"),

		// Packet entered the server from a verified client-only connection
		// and the FROMCALL does not match the login:
		b.pass("(1) qAR,login becomes qAo",
			"SRCCALL>DST,DIGI1*,qAR,"+login+":testing (1)",
			"SRCCALL>DST,DIGI1*,qAo,"+login+":testing (1)"),
		b.pass("(2) login,I becomes qAo,login",
			"SRCCALL>DST,DIGI1*,"+login+",I:testing (2)",
			"SRCCALL>DST,DIGI1*,qAo,"+login+":testing (2)"),
		b.pass("(3) VIACALL,I becomes qAr,VIACALL",
			"SRCCALL>DST,DIGI1*,IGATE,I:testing (3)",
			"SRCCALL>DST,DIGI1*,qAo,IGATE:testing (3)"),
		b.pass("(4) qAO,login appended",
			"SRCCALL>DST,DIGI1*:testing (4)",
			"SRCCALL>DST,DIGI1*,qAO,"+login+":testing (4)"),
		// (5) any other (even unknown) q construct is passed intact
		b.pass("(5) other q construct passed intact",
			"SRCCALL>DST,DIGI1*,qAF,"+login+":testing (5)",
			"SRCCALL>DST,DIGI1*,qAF,"+login+":testing (5)"),
	}
}

// qcFullFeedPhase2 are the q construct cases for a verified client on a
// regular port. second is the callsign of another client, which is logged in
// to test loop detection.
func qcFullFeedPhase2(login, second, proto string) []isclient.Step {
	b := qBuilder{proto}

	return []isclient.Step{
		// Header terminated with ,I: if the VIACALL preceding the ,I matches
		// the login, change to ,qAR,VIACALL, else to ,qAr,VIACALL.
		b.pass("(b1) login,I becomes qAR,login",
			"SRC>DST,DIGI1,DIGI5*,"+login+",I:Asdf (b1)",
			"SRC>DST,DIGI1,DIGI5*,qAR,"+login+":Asdf (b1)"),
		b.pass("(b2) VIACALL,I becomes qAr,VIACALL",
			"SRC>DST,DIGI1,DIGI5*,N5CAL,I:Asdf (b2)",
			"SRC>DST,DIGI1,DIGI5*,qAr,N5CAL:Asdf (b2)"),

		// Else if the FROMCALL matches the login: append ,qAC,SERVERLOGIN
		// and quit q processing. Only one TCPIP* should be inserted.
		b.pass("fromcall matches login",
			login+">DST:aifyua",
			login+">DST,TCPIP*,qAC,"+serverCall+":aifyua"),
		b.pass("fromcall matches login, TCPIP* present",
			login+">DST,TCPIP*:gaaee",
			login+">DST,TCPIP*,qAC,"+serverCall+":gaaee"),
		// Else append ,qAS,login
		b.pass("qAS,login appended",
			"SRC>DST,DIGI1,DIGI2*:test",
			"SRC>DST,DIGI1,DIGI2*,qAS,"+login+":test"),

		// All packets with q constructs: if ,qAZ, is the q construct, the
		// packet is dropped.
		b.drop("qAZ dropped",
			"SRCCALL>DST,DIGI1*,qAZ,"+login+":testing (qAZ)"),
		// If ,SERVERLOGIN is found after the q construct, drop.
		b.drop("server login after q construct",
			"SRCCALL>DST,DIGI1*,qAR,"+serverCall+":testing (,SERVERLOGIN)"),
		// If a callsign-SSID is found twice in the q construct, drop.
		b.drop("call twice in q construct",
			"SRCCALL>DST,DIGI1*,qAI,FOOBAR,ASDF,ASDF,BARFOO:testing (dup call)"),
		// If a verified login other than this login is found in the q
		// construct and that login is not allowed to have multiple verified
		// connects, drop. The second connection is logged in as second.
		b.drop("verified call loop",
			"SRCCALL>DST,DIGI*,qAI,"+second+","+login+":testing (verified call loop)"),
		// If the packet is from an inbound port and the login is found after
		// the q construct but is not the LAST VIACALL, drop.
		b.drop("login not last viacall",
			"SRCCALL>DST,DIGI*,qAI,"+login+",M0RE:testing (login not last viacall)"),

		// If the q construct is qAI: if the login is not found after the q
		// construct, append ,login (1), then append ,SERVERLOGIN (3).
		b.pass("qAI (1) login appended",
			"SRC>DST,DIGI1,DIGI2*,qAI,FOOBAR:testing qAI (1)",
			"SRC>DST,DIGI1,DIGI2*,qAI,FOOBAR,"+login+","+serverCall+":testing qAI (1)"),
		b.pass("qAI (3) server login appended",
			"SRC>DST,DIGI1,DIGI2*,qAI,"+login+":testing qAI (3)",
			"SRC>DST,DIGI1,DIGI2*,qAI,"+login+","+serverCall+":testing qAI (3)"),

		// qAS appending bug in javaprssrvr 3.15: a packet from a broken DPRS
		// gateway with no dstcall got a new qAS,login appended at every
		// javaprssrvr on the way.
		b.pass("qAS not appended repeatedly",
			"K1FRA>qAR,K1RFI-C,qAS,"+login+":/281402z4144.72N/07125.65W>178/001",
			"K1FRA>qAR,K1RFI-C,qAS,"+login+":/281402z4144.72N/07125.65W>178/001"),
	}
}

// q construct handling for a verified client on a client-only port, and then
// on a regular port. Port of t/20qconstr-clientonly.t.
func TestQConstrClientOnly(t *testing.T) {
	harness.Parallel(t)
	testQConstrClientOnly(t, "basic", "A", "O")
}

// The same with the q construct protocol id configured as O. Port of
// t/24qconstr-protocols.t.
func TestQConstrProtocols(t *testing.T) {
	harness.Parallel(t)
	testQConstrClientOnly(t, "qconstr-protocols", "O", "A")
}

func testQConstrClientOnly(t *testing.T, cfg, proto, wrong string) {
	s := harness.Start(t, cfg)

	const login = "N5CAL-1"
	const second = "MYC4LL-5"

	tx := isclient.Connect(t, harness.Addr(t, 55581), login)
	rx := isclient.Connect(t, harness.Addr(t, 55152), "N5CAL-2")

	t.Run("client-only port", func(t *testing.T) {
		isclient.RunSteps(t, tx, rx, qcClientOnlyPhase1(login, proto, wrong))
	})

	// reconnect to the non-client-only port
	if err := tx.Close(); err != nil {
		t.Errorf("failed to disconnect from the server: %v", err)
	}
	tx = isclient.Connect(t, harness.Addr(t, 55152), login)

	// for loop testing, also make a second connection
	isclient.Connect(t, harness.Addr(t, 55152), second)

	t.Run("full feed port", func(t *testing.T) {
		isclient.RunSteps(t, tx, rx, qcFullFeedPhase2(login, second, proto))
	})

	s.Stop()
}

// q construct handling for a verified client on an igate port, and then on a
// regular port. Port of t/20qconstr-verified.t.
func TestQConstrVerified(t *testing.T) {
	harness.Parallel(t)
	s := harness.Start(t, "basic")

	const login = "N5CAL-1"
	const second = "MYC4LL-5"
	const proto = "A"

	tx := isclient.Connect(t, harness.Addr(t, 55580), login)
	rx := isclient.Connect(t, harness.Addr(t, 55152), "N5CAL-5")
	b := qBuilder{proto}

	t.Run("igate port", func(t *testing.T) {
		isclient.RunSteps(t, tx, rx, []isclient.Step{
			b.pass("tcpip-path-replace1",
				login+">DST:tcpip-path-replace1",
				login+">DST,TCPIP*,qAC,TESTING:tcpip-path-replace1"),
			b.pass("tcpip-path-replace2",
				login+">DST,DIGI1,DIGI5*:tcpip-path-replace2",
				login+">DST,TCPIP*,qAC,TESTING:tcpip-path-replace2"),
			b.pass("tcpip-path-replace3",
				login+">DST,TCPIP*:tcpip-path-replace3",
				login+">DST,TCPIP*,qAC,TESTING:tcpip-path-replace3"),
			b.pass("tcpip-path-replace4",
				login+">DST,qAR,"+login+":tcpip-path-replace4",
				login+">DST,TCPIP*,qAC,TESTING:tcpip-path-replace4"),
			b.pass("tcpip-path-replace5",
				login+">DST,WIDE2-2,qAR,"+login+":tcpip-path-replace5",
				login+">DST,TCPIP*,qAC,TESTING:tcpip-path-replace5"),

			// A q construct without a call after it is deleted, and qAS added.
			b.pass("q construct without a call",
				"SRC>DST,DIGI1,DIGI5*,qAR:a4ufy",
				"SRC>DST,DIGI1,DIGI5*,qAS,"+login+":a4ufy"),
			b.drop("star after q construct",
				"SRCCALL>DST,DIGI1*,qAR,GATES*:testing * after Q construct"),

			// Packet entered the server from a verified connection and the
			// FROMCALL does not match the login:
			b.pass("(1) qAR,login passed intact",
				"SRCCALL>DST,DIGI1*,qAR,"+login+":testing (1)",
				"SRCCALL>DST,DIGI1*,qAR,"+login+":testing (1)"),
			b.pass("(2) login,I becomes qAR,login",
				"SRCCALL>DST,DIGI1*,"+login+",I:testing (2)",
				"SRCCALL>DST,DIGI1*,qAR,"+login+":testing (2)"),
			b.pass("(3) VIACALL,I becomes qAr,VIACALL",
				"SRCCALL>DST,DIGI1*,IGATE,I:testing (3)",
				"SRCCALL>DST,DIGI1*,qAr,IGATE:testing (3)"),
			b.pass("(4) qAS,login appended",
				"SRCCALL>DST,DIGI1*:testing (4)",
				"SRCCALL>DST,DIGI1*,qAS,"+login+":testing (4)"),
			b.pass("(5) other q construct passed intact",
				"SRCCALL>DST,DIGI1*,qAF,"+login+":testing (5)",
				"SRCCALL>DST,DIGI1*,qAF,"+login+":testing (5)"),
		})
	})

	// reconnect to a full-feed port
	if err := tx.Close(); err != nil {
		t.Errorf("failed to disconnect from the server: %v", err)
	}
	tx = isclient.Connect(t, harness.Addr(t, 55152), login)

	// for loop testing, also make a second connection
	isclient.Connect(t, harness.Addr(t, 55152), second)

	t.Run("full feed port", func(t *testing.T) {
		steps := qcFullFeedPhase2(login, second, proto)
		steps = append(steps,
			// INERR_Q_QPATH_CALL_TWICE
			b.drop("same callsign twice in q path",
				"SRCCALL>DST,qAI,FOO1,FOO1,"+login+":Same callsign twice in Q path"),
			// INERR_Q_I_NO_VIACALL
			b.drop("old-style I path with no viacall",
				"SRCCALL>DST,I:Old-style I path with no viacall"),
		)
		isclient.RunSteps(t, tx, rx, steps)
	})

	s.Stop()
}

// Path tracing (qAI). Port of t/22path-trace.t.
func TestQConstrPathTrace(t *testing.T) {
	harness.Parallel(t)
	s := harness.Start(t, "basic")

	const txCall = "OHCALL-1"
	tx := isclient.Connect(t, harness.Addr(t, 55152), txCall)
	rx := isclient.Connect(t, harness.Addr(t, 55152), "OHCALL-2")

	// If the q construct is qAI: (1) if the login is not found after it,
	// append ,login, then (3) append ,SERVERLOGIN.
	position := "!6028.52N/02505.61E# Testing"
	isclient.RunSteps(t, tx, rx, []isclient.Step{
		pass("(1) login appended",
			txCall+">DST,DIGI,qAI,BLAH:"+position,
			txCall+">DST,DIGI,qAI,BLAH,"+txCall+","+serverCall+":"+position),
		// make it unique to pass the dupe check
		pass("(3) server login appended",
			txCall+">DST,DIGI,qAI,"+txCall+":"+position+".",
			txCall+">DST,DIGI,qAI,"+txCall+","+serverCall+":"+position+"."),
	})

	s.Stop()
}
