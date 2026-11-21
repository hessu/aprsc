package functional

import (
	"strings"
	"testing"
	"time"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// Miscellaneous bad packets which must be dropped: anything from an unverified
// client, RFONLY and NOGATE in the path, third-party packets with TCPIP or
// TCPXX in the inner path, general queries, invalid characters and callsigns,
// disallowed source and igate callsigns, a disallowed q construct, DX spots,
// and messages to KipSS and javaMSG. Port of t/11misc-drops.t.
//
// Unlike the drop steps elsewhere, all the packets are sent first, as the Perl
// test did, and then everything the full feed sends within a short time is
// checked: only the last packet, which is valid, may come out.
func TestMiscDrops(t *testing.T) {
	harness.Parallel(t)
	s := harness.Start(t, "basic")

	const login = "N5CAL-1"
	tx := isclient.Connect(t, harness.Addr(t, 55580), login)
	rx := isclient.Connect(t, harness.Addr(t, 55152), "N5CAL-2")

	const unverCall = "N5UN-1"
	un := isclient.Connect(t, harness.Addr(t, 55580), unverCall, isclient.WithPasscode("-1"))

	// the unverified client must not get anything through at all
	for _, p := range []string{
		"OH2XXX>APRS,qAR," + login + ":>should drop from unverified client",
		unverCall + ">APRS,qAR," + unverCall + ":>should drop from unverified client, 2",
		unverCall + ">APRS,qAR," + unverCall + ":!6028.52N/02505.61E# Testing",
	} {
		un.Send(t, p)
	}

	// other drop reasons
	drops := []string{
		"SRC>APRS,RFONLY,qAR," + login + ":>should drop, RFONLY",
		"SRC>APRS,NOGATE,qAR," + login + ":>should drop, NOGATE",
		"SRC>APRS,RFONLY,qAR," + login + ":>should drop, RFONLY",
		"SRC>DST,DIGI,qAR," + login + ":}SRC2>DST,DIGI,TCPIP*:>should drop, 3rd party",
		"SRC>DST,DIGI,qAR," + login + ":}SRC3>DST,DIGI,TCPXX*:>should drop, 3rd party TCPXX",
		"SRC>DST,DIGI,qAR," + login + ":?APRS? general query",
		"SRC>DST,DIGI,qAR," + login + ":?WX? general query",
		"SRC>DST,DIGI,qAR," + login + ":?FOOBAR? general query",
		"SRC>DST\x08,DIGI,qAR," + login + ":>should drop ctrl-B in dstcall",
		"SRC\x08>DST,DIGI,qAR," + login + ":>should drop ctrl-B in srccall",
		" SRC>DST,DIGI,qAR," + login + ":>should drop, space in front of srccall",
		"\x00SRC>DST,DIGI,qAR," + login + ":>should drop, null in front of srccall",
		"S\\RC>DST,DIGI,qAR," + login + ":>should drop, backslash in srccall",
		"S\xefRC>DST,DIGI,qAR," + login + ":>should drop, high byte in srccall",
		"SRCXXXXXXX>APRS,qAR," + login + ":>should drop, too long srccall",
		"SRC>APRSXXXXXX,qAR," + login + ":>should drop, too long dstcall",
		"SRC>APRS,OH2DIGI-12,qAR," + login + ":>should drop, too long call in path",
		"SRC>APT311,RELAY,WIDE,WIDE/V,qAR," + login + ":!4239.93N/08254.93Wv342/000 should drop, / in digi path",
		"SRC>DST,DIG*I,qAR," + login + ":>should drop, * in middle of digi call",
		"SRC>DST,DI\x08GI,qAR," + login + ":>should drop, ctrl-B in middle of digi call",
		"SRC2>DST,DIGI,qAX," + login + ":>Packet from unverified login according to qAX",
		"SRC2>DST,DIGI,TCPXX,qAR," + login + ":>Packet from unverified login according to TCPXX in path",
		"SRC2>DST,DIGI,TCPXX*,qAR," + login + ":>Packet from unverified login according to TCPXX* in path",
		"SRC->DST,DIGI-0,qAR," + login + ":>should drop, too short SSID in srccall",
		// javAPRSSrvr 4 also drops a too long SSID (SRC-111, E1E-DH5FF) and a
		// too short callsign (EL, EL-1); javAPRSSrvr 3 and aprsc allow them,
		// and all of them allow really long source callsigns (OH5FFLONG).
		// disallowed source callsigns
		"N0CALL>DST,DIGI,qAR," + login + ":>should drop, N0CALL as source callsign",
		"NOCALL>DST,DIGI,qAR," + login + ":>should drop, NOCALL as source callsign",
		"NOCALL-1>DST,DIGI,qAR," + login + ":>should drop, N0CALL-1 as source callsign",
		"SERVER>DST,DIGI,qAR," + login + ":>should drop, SERVER as source callsign",
		// additionally configured disallowed source callsigns: N7CALL N8CALL* *DROP DRG* OH?DRU
		"N7CALL>DST,DIGI,qAR," + login + ":>should drop, N7CALL as source callsign matches N7CALL",
		"N8CALL>DST,DIGI,qAR," + login + ":>should drop, N8CALL as source callsign matches N8CALL*",
		"GLDROP>DST,DIGI,qAR," + login + ":>should drop, GLDROP as source callsign matches *DROP",
		"DRGLOB>DST,DIGI,qAR," + login + ":>should drop, DRGLOB as source callsign matches DRG*",
		"OH7DRU>DST,DIGI,qAR," + login + ":>should drop, OH7DRU as source callsign matches OH?DRUP",
		// disallowed igate/receiver callsigns after the Q construct: IGDROP IG8CALL* *IGREJ IGR* OH?IGA
		"SRC>DST,DIGI,qAR,IGDROP:>should drop, IGDROP as igate callsign matches IGDROP",
		"SRC>DST,DIGI,qAR,IG8CALL-1:>should drop, IG8CALL-1 as igate callsign matches IG8CALL*",
		"SRC>DST,DIGI,qAR,XIGREJ:>should drop, XIGREJ as igate callsign matches *IGREJ",
		"SRC>DST,DIGI,qAR,IGROTHER:>should drop, IGROTHER as igate callsign matches IGR*",
		"SRC>DST,DIGI,qAR,OH2IGA:>should drop, OH2IGA as igate callsign matches OH?IGA",
		// disallowed Q construct itself
		"SRC>DST,DIGI,qAY,IGTEST:>should drop, qAY Q construct matches disallowed qAY",
		// DX spots
		"SRC>DST,DIGI,qAR," + login + ":DX de FOO: BAR - should drop",
		// disallowed message recipients, status messages and such
		"SRC-2>APRS,qAR," + login + "::KIPSS    :KipSS Login:OK1xx-1{16",
		"SRC>APRS,qAR," + login + "::javaMSG  :Foo_BAR Linux APRS Server: 192.168.10.72 connected 2 users online.",
	}
	for _, p := range drops {
		tx.Send(t, p)
	}

	// A valid packet, with a 32-character IPv6 address as a path element,
	// which must come out: it proves that the previous ones were dropped and
	// not just delayed.
	pass := "OH2SRC>APRS,OH2DIG-12*,OH2DIG-1*,qAR,200106F8020204020000000000000002," + login + ":should pass"
	tx.Send(t, pass)

	// read everything that comes out, until the feed has been quiet for a
	// while
	passed := false
	for {
		got, err := rx.ReadPacket(500 * time.Millisecond)
		if err != nil {
			if !isclient.IsTimeout(err) {
				t.Fatalf("full feed connection failed: %v", err)
			}
			break
		}
		if got == pass {
			passed = true
			continue
		}
		t.Errorf("server forwarded a packet it should have dropped: %q", got)
	}
	if !passed {
		t.Errorf("the valid packet %q did not come out", pass)
	}

	s.Stop()
}

// A client whose packets are dropped for many different reasons: when it
// disconnects, the server logs a summary of the reasons, which must not get
// longer than the buffer it is built in.
func TestManyDropReasons(t *testing.T) {
	harness.Parallel(t)
	harness.RequireAprsc(t)
	s := harness.Start(t, "basic")

	const login = "N5CAL-1"
	tx := isclient.Connect(t, harness.Addr(t, 55152), login)
	rx := isclient.Connect(t, harness.Addr(t, 55152), "N5CAL-2")

	for _, p := range []string{
		"SRC>APRS no colon in this packet",
		"SRC APRS:>no dst",
		"SR*C>APRS:>invalid srccall",
		"SRCCALL>APRS:",
		"SRC>AP*RS:>invalid dstcall",
		"SRC>APRS,NOGATE:>nogate",
		"SRC>APRS:}A>B,TCPIP,C*:>3rd party from the Internet",
		"SRC>APRS:}A>B:>invalid 3rd party",
		"SRC>APRS:?APRS?",
		"A>B:x",
		"SRC>APRS:>" + strings.Repeat("long ", 120),
		"SRC>APRS,DI*GI:>invalid path call",
		"SRC>APRS,qAZ,FOO:>qAZ",
		"SRC>APRS,qAR," + serverCall + ":>my call in path",
		"SRC>APRS,qAR,FOO,FOO:>call twice",
		"SRC>APRS,qAR," + login + ",FOO:>login not last",
		"SRC>APRS,qAU,SRC:>qAU srccall",
		"SRC>APRS,I:>no viacall",
		"N0CALL>APRS:>disallowed srccall",
		"SRC>APRS:DX de FOO",
		"SRC>APRS::javaMSG  :disallowed message recipient",
		"SRC>APRS,WIDE1-1,qAC,OTHER:>qAC not TCPIP",
	} {
		tx.Send(t, p)
	}

	// the drops have been processed when this comes through
	isclient.TxRx(t, tx, rx, "SRC>APRS,qAR,"+login+":>after the drops", "SRC>APRS,qAR,"+login+":>after the drops")

	tx.Close()

	// wait for the server to have closed the client
	deadline := time.Now().Add(8 * time.Second)
	for {
		_, clients := statusWithClients(t, 1)
		if _, there := clients[login]; !there {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("%s is still in the status after closing", login)
		}
		time.Sleep(250 * time.Millisecond)
	}

	// and the server is still there
	tx2 := isclient.Connect(t, harness.Addr(t, 55152), "N5CAL-3")
	isclient.TxRx(t, tx2, rx, "SRC>APRS,qAR,N5CAL-3:>after the close", "SRC>APRS,qAR,N5CAL-3:>after the close")

	s.Stop()
}
