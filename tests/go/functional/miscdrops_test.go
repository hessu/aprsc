package functional

import (
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
	s := harness.Start(t, "basic")

	const login = "N5CAL-1"
	tx := isclient.Connect(t, "localhost:55580", login)
	rx := isclient.Connect(t, "localhost:55152", "N5CAL-2")

	const unverCall = "N5UN-1"
	un := isclient.Connect(t, "localhost:55580", unverCall, isclient.WithPasscode("-1"))

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
