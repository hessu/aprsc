package functional

import (
	"fmt"
	"strings"
	"testing"
	"time"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// Disconnection in the middle of transmitting a packet, a blob of many packets
// in a single write, and thousands of short lines in a single write. Port of
// t/50disc-blobs.t.
func TestDiscBlobs(t *testing.T) {
	s := harness.Start(t, "basic")

	const loginTx = "N0GAT"
	tx := isclient.Connect(t, "localhost:55580", loginTx)
	rx := isclient.Connect(t, "localhost:55152", "N1GAT")

	// Send a packet halfway and disconnect. The server must not forward it.
	if err := tx.SendRaw([]byte("M0SRC>APRS,OH2RDG*,WIDE,qAR," + loginTx + ":!6028.51N/02505.68E#this packet trunca")); err != nil {
		t.Fatalf("failed to send a partial packet: %v", err)
	}
	time.Sleep(time.Second) // let the server read it before the disconnect
	tx.Close()

	isclient.ExpectSilence(t, rx, 2*time.Second, "truncated packet")

	// connect again
	tx = isclient.Connect(t, "localhost:55580", loginTx)

	t.Run("blob", func(t *testing.T) {
		const blobSize = 32 * 1024

		var packets []string
		var blob strings.Builder
		txl := 0
		for txl < blobSize {
			p := fmt.Sprintf("M%d>APRS,qAR,%s:!6028.51N/02505.68E#should pass blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa END",
				len(packets)+10, loginTx)
			packets = append(packets, p)
			blob.WriteString(p + "\r\n")
			txl += len(p)
		}
		t.Logf("blob size %d (%d) with %d packets", txl, blobSize, len(packets))

		if err := tx.SendRaw([]byte(blob.String())); err != nil {
			t.Fatalf("failed to transmit blob of %d bytes: %v", txl, err)
		}

		rxn, rxl := 0, 0
		for {
			got, err := rx.ReadPacket(time.Second)
			if err != nil {
				break
			}
			if rxn < len(packets) && got != packets[rxn] {
				t.Errorf("got wrong packet %d: %q", rxn, got)
			}
			rxn++
			rxl += len(got)
		}

		if rxn != len(packets) {
			t.Errorf("received %d lines from blob, want %d", rxn, len(packets))
		}
		if rxl != txl {
			t.Errorf("received %d bytes from blob, want %d", rxl, txl)
		}
	})

	// Thousands of short lines in a single write: the deframer must handle a
	// buffer full of lines without growing the worker thread's stack.
	t.Run("short lines", func(t *testing.T) {
		if err := tx.SendRaw([]byte(strings.Repeat("a\n", 8000))); err != nil {
			t.Fatalf("failed to transmit 8000 short lines: %v", err)
		}

		// the server must still be there and forwarding packets
		p := "M1SRC>APRS,qAR," + loginTx + ":!6028.51N/02505.68E#after short lines"
		isclient.TxRx(t, tx, rx, p, p)
	})

	s.Stop()
}

// Heavier packet load: 20 MB of packets through the server, in order, with a
// limited number of bytes outstanding. Port of t/51load.t.
func TestLoad(t *testing.T) {
	s := harness.Start(t, "basic")

	const loginTx = "N0GAT"
	rx := isclient.Connect(t, "localhost:55152", "N1GAT")
	tx := isclient.Connect(t, "localhost:55580", loginTx)

	time.Sleep(500 * time.Millisecond) // let it get started

	const flushInterval = 300
	const byteLimit = 20 * 1024 * 1024
	const window = 64 * 1024

	packet := func(n int) string {
		return fmt.Sprintf("M%d>APRS,qAR,%s:!6028.51N/02505.68E# packet %d blaa blaa blaa blaa END",
			n%10000+10, loginTx, n)
	}

	var txq strings.Builder
	outstanding, txn, rxn, txl, rxl := 0, 0, 0, 0, 0
	wrong := 0

	received := func(line string) {
		if line != packet(rxn) {
			if wrong++; wrong <= 5 {
				t.Errorf("received wrong packet %d: %q", rxn, line)
			}
		}
		outstanding -= len(line) + 2
		rxn++
		rxl += len(line) + 2
	}

	flush := func() {
		if err := tx.SendRaw([]byte(txq.String())); err != nil {
			t.Fatalf("failed to send: %v", err)
		}
		outstanding += txq.Len()
		txq.Reset()
	}

	start := time.Now()

	for txl < byteLimit {
		p := packet(txn) + "\r\n"
		txl += len(p)
		txq.WriteString(p)
		txn++

		if txq.Len() >= flushInterval {
			flush()
		}

		for outstanding > window {
			line, err := rx.ReadPacket(time.Second)
			if err != nil {
				break
			}
			received(line)
		}
	}

	if txq.Len() > 0 {
		flush()
	}

	t.Logf("reading the rest, have received %d packets, sent %d", rxn, txn)
	for outstanding > 0 {
		line, err := rx.ReadPacket(500 * time.Millisecond)
		if err != nil {
			break
		}
		received(line)
	}
	t.Logf("after reading the rest, have received %d packets, sent %d, outstanding %d bytes", rxn, txn, outstanding)

	dur := time.Since(start)
	t.Logf("took %.3f seconds, %.0f packets/sec", dur.Seconds(), float64(rxn)/dur.Seconds())

	if rxn != txn {
		t.Errorf("received %d lines, sent %d", rxn, txn)
	}
	if rxl != txl {
		t.Errorf("received %d bytes, sent %d", rxl, txl)
	}
	if outstanding != 0 {
		t.Errorf("%d bytes outstanding in the server after timeout", outstanding)
	}

	s.Stop()
}
