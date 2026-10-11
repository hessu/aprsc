package functional

import (
	"fmt"
	"testing"
	"time"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// msgTo returns the "::DEST     :" part of a message packet, with the
// addressee padded to 9 characters.
func msgTo(dest string) string {
	return fmt.Sprintf("::%-9.9s:", dest)
}

// duplex is a client which can both send and receive packets, over whichever
// protocol.
type duplex interface {
	isclient.Sender
	isclient.Receiver
}

// Messaging features:
//
// On a filtered igate port, no messages should come out at first. When a
// position of a station has been heard, messages for that station should come
// out. After such a message, the next following position transmitted within
// 30 minutes by the originator should be passed to the recipient's socket,
// too.
//
// When a position has been heard, positions for the same callsign-ssid from
// other igates should come out too, to assist TX igates to know the station
// is on the Internet.
//
// Port of t/40messaging.t.
func TestMessaging(t *testing.T) {
	harness.Parallel(t)
	s := harness.Start(t, "basic")

	const filter = "r/60.4752/25.0947/1"

	// A filter is set on both so that the helper packets get through.
	tx := isclient.Connect(t, harness.Addr(t, 55580), "N0GATE", isclient.WithFilter(filter))
	rx := isclient.Connect(t, harness.Addr(t, 55580), "N1GATE", isclient.WithFilter(filter))

	messaging(t, tx, rx, func() duplex {
		return isclient.Connect(t, harness.Addr(t, 55580), "N2GATE", isclient.WithFilter(filter))
	})

	// The g/ message recipient filter.
	t.Run("g filter", func(t *testing.T) {
		rx.SetFilter(t, "g/PREF*/FULLCAL")

		// the first message does not match the filter, the second one does
		isclient.ExpectDrop(t, tx, rx,
			"OH1SRC>APRS,OH2RDG*,WIDE,N0GATE,I"+msgTo("NOMATCH")+"message",
			"OH1SRC>APRS,OH2RDG*,WIDE,N0GATE,I"+msgTo("FULLCAL")+"message")

		// wildcard in the g/ filter
		again := func(to string) string {
			return "OH1SRC>APRS,qAR,N0GATE" + msgTo(to) + "message again"
		}
		isclient.TxRx(t, tx, rx, again("PREFIX"), again("PREFIX"))
		isclient.TxRx(t, tx, rx, again("PREF"), again("PREF"))
	})

	s.Stop()
}

// messaging runs the messaging scenario between two connected igates, tx
// (N0GATE) and rx (N1GATE), and a third one made by connectRx2 (N2GATE). All
// of them have a range filter around the stations' positions.
func messaging(t *testing.T, tx, rx duplex, connectRx2 func() duplex) {
	const loginTx = "N0GATE"
	const loginRx = "N1GATE"
	const msgSrc = "M1SRC"
	const msgDst = "M1DST"
	const path = ">APRS,OH2RDG*,WIDE,"
	const pos = ":!6028.51N/02505.68E# "

	// txrx runs a pass step in the given direction.
	txrx := func(name string, from, to duplex, sent, want string) {
		t.Run(name, func(t *testing.T) { isclient.TxRx(t, from, to, sent, want) })
	}
	dropped := func(name string, from, to duplex, sent, helper string) {
		t.Run(name, func(t *testing.T) { isclient.ExpectDrop(t, from, to, sent, helper) })
	}

	// A message packet is not passed to a filtered port.
	dropped("message not passed to filtered port", tx, rx,
		msgSrc+path+loginTx+",I"+msgTo(msgDst)+"message",
		"H1LP>APRS,OH2RDG*,WIDE"+pos+"should pass")

	// A position packet from the message originator is not passed to a
	// filtered port.
	dropped("originator position not passed", tx, rx,
		msgSrc+path+loginTx+",I:!6428.51N/02545.98E#",
		"H1LP-5>APRS,OH2RDG*,WIDE"+pos+"should pass")

	// Transmit position packets on the receiving filtered port.
	txrx("position of M1DST-2", rx, tx,
		msgDst+"-2"+path+loginRx+",I"+pos+"should pass",
		msgDst+"-2"+path+"qAR,"+loginRx+pos+"should pass")

	time.Sleep(time.Second)

	txrx("position of M1DST", rx, tx,
		msgDst+path+loginRx+",I"+pos+"should pass",
		msgDst+path+"qAR,"+loginRx+pos+"should pass")
	txrx("position of M1DST-3", rx, tx,
		msgDst+"-3"+path+loginRx+",I"+pos+"should pass",
		msgDst+"-3"+path+"qAR,"+loginRx+pos+"should pass")

	// Then a message packet should magically pass!
	txrx("message passes", tx, rx,
		msgSrc+path+loginTx+",I"+msgTo(msgDst)+"message{123",
		msgSrc+path+"qAR,"+loginTx+msgTo(msgDst)+"message{123")

	// And its ACK packet should pass, too.
	txrx("ack passes", rx, tx,
		msgDst+path+loginRx+",I"+msgTo(loginTx)+"ack123",
		msgDst+path+"qAR,"+loginRx+msgTo(loginTx)+"ack123")

	// Another message, with UTF-8 content.
	txrx("utf-8 message", tx, rx,
		msgSrc+path+loginTx+",I"+msgTo(msgDst)+"Blää  blåå 日本語{1d",
		msgSrc+path+"qAR,"+loginTx+msgTo(msgDst)+"Blää  blåå 日本語{1d")

	// Another message, with high-value binary content.
	binary := msgSrc + path + "qAR," + loginTx + msgTo(msgDst) + "binary "
	for d := 127; d <= 255; d++ {
		binary += string([]byte{byte(d)})
	}
	txrx("binary message", tx, rx, binary, binary)

	// It does not pass to another SSID. (javaprssrvr does not pass it either.)
	dropped("message to another ssid", tx, rx,
		msgSrc+path+loginTx+",I"+msgTo(msgDst+"-5")+"message with SSID{a",
		"H1LP>APRS,OH2RDG*,WIDE"+pos+"should pass5")

	// After a message has been transmitted, the complimentary position
	// should pass.
	txrx("complimentary position", tx, rx,
		msgSrc+path+loginTx+",I:!5528.51N/00505.68E# should pass compl",
		msgSrc+path+"qAR,"+loginTx+":!5528.51N/00505.68E# should pass compl")

	// A second complimentary position must be dropped.
	dropped("second complimentary position", tx, rx,
		msgSrc+path+loginTx+",I:!5628.51N/00505.68E# should drop compl2",
		"H1LP-C>APRS,OH2RDG*,WIDE"+pos+"should pass")

	// A position packet having TCPIP* in the path, coming from another
	// connection, should be passed to the port where the station was heard
	// without TCPIP*.
	//
	// "This is so the IGate can determine if there is a station it is hearing
	// on RF is also directly connected to APRS-IS. If the RF station is heard
	// directly on APRS-IS, the IGate should NOT gate messages for that station
	// to RF." (Pete Loveall, 17 May 2012, APRSSIG)
	tcpip := msgSrc + ">APRS,TCPIP*,qAC," + msgSrc + ":!5528.51N/00505.68E# should pass TCPIP*"
	txrx("position with TCPIP*", rx, tx, tcpip, tcpip)

	// Message to the client's callsign.
	txrx("message to client callsign", tx, rx,
		msgSrc+path+loginTx+",I"+msgTo(loginRx)+"message",
		msgSrc+path+"qAR,"+loginTx+msgTo(loginRx)+"message")
	dropped("message to client callsign with another ssid", tx, rx,
		msgSrc+path+loginTx+",I"+msgTo(loginRx+"-9")+"message with ssid",
		"H1LP>APRS,OH2RDG*,WIDE"+pos+"should pass diff ssid")

	// Message to an object: transmit the object on the receiving filtered
	// port. The message should not pass at the moment.
	obj := fmt.Sprintf(":;%-9.9s*111111z6028.51N/02505.68Ercomment", "OBJDST")
	txrx("object", rx, tx,
		msgDst+path+loginRx+",I"+obj,
		msgDst+path+"qAR,"+loginRx+obj)
	dropped("message to object", tx, rx,
		msgSrc+path+loginTx+",I"+msgTo("OBJDST")+"message to object",
		"H1LP>APRS,OH2RDG*,WIDE"+pos+"should pass6")

	// Message to an item: transmit the item on the receiving filtered port.
	// The message should not pass at the moment.
	item := ":)ITEDST!6028.51N/02505.68Ercomment"
	txrx("item", rx, tx,
		msgDst+path+loginRx+",I"+item,
		msgDst+path+"qAR,"+loginRx+item)
	dropped("message to item", tx, rx,
		msgSrc+path+loginTx+",I"+msgTo("ITEDST")+"message to item",
		"H1LP>APRS,OH2RDG*,WIDE"+pos+"should pass7")

	// Connect another igate and see what happens when there are two gates
	// hearing the same station.
	rx2 := connectRx2()

	// A position packet on the second receiving filtered port comes out on
	// the first receiving filtered port due to the range filter *and* due to
	// it being heard there, too.
	t.Run("position heard by two gates", func(t *testing.T) {
		want := msgDst + path + "qAr," + loginRx + pos + "should pass 2nd"
		isclient.TxRx(t, rx2, tx, msgDst+path+loginRx+",I"+pos+"should pass 2nd", want)

		got, err := rx.ReadPacket(isclient.DefaultTimeout)
		if err != nil || got != want {
			t.Errorf("first rx port got %q (%v), want %q", got, err, want)
		}
	})

	// A message packet should magically pass to both.
	t.Run("message to two gates", func(t *testing.T) {
		want := msgSrc + path + "qAR," + loginTx + msgTo(msgDst) + "two gates"
		isclient.TxRx(t, tx, rx2, msgSrc+path+loginTx+",I"+msgTo(msgDst)+"two gates", want)

		got, err := rx.ReadPacket(isclient.DefaultTimeout)
		if err != nil || got != want {
			t.Errorf("first rx port got %q (%v), want %q", got, err, want)
		}
	})
}
