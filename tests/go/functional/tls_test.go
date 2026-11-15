package functional

import (
	"crypto/tls"
	"fmt"
	"net"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// holdConn is a net.Conn which, while holding, collects everything written to
// it and sends it all in a single write when released. Over TLS this packs
// several records, one per Write of the TLS client, into one TCP write, as
// happens on a real network when a client sends many packets in a burst.
type holdConn struct {
	net.Conn
	mu      sync.Mutex
	holding bool
	held    []byte
}

func (h *holdConn) Write(b []byte) (int, error) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.holding {
		h.held = append(h.held, b...)
		return len(b), nil
	}
	return h.Conn.Write(b)
}

// Hold makes the following writes wait for Release.
func (h *holdConn) Hold() {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.holding = true
}

// Release sends the held writes in a single write and stops holding.
func (h *holdConn) Release() error {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.holding = false
	held := h.held
	h.held = nil
	_, err := h.Conn.Write(held)
	return err
}

// A TLS client sends a burst of packets, each in a TLS record of its own,
// which arrive at the server in a single TCP segment. All of them must be
// passed on to other clients right away. OpenSSL reads ahead, so a single
// socket read picks up all the records, and SSL_read() returns only one of
// them at a time; the server used to read one record per poll event, so the
// rest of the burst sat in OpenSSL's buffer until the client sent something
// more. Reported with 16 packets of which 13 to 15 arrived.
func TestTLSRecordBurst(t *testing.T) {
	harness.Parallel(t)
	harness.RequireAprsc(t)
	harness.Start(t, "basic")

	rx := isclient.Connect(t, harness.Addr(t, 55152), "N0RX")

	// the TLS listeners require a client certificate; the test suite's
	// one has the callsign N5CAL
	certDir := filepath.Join(harness.TestsDir(), "cfg-aprsc")
	caCert, err := tls.LoadX509KeyPair(filepath.Join(certDir, "tls-client-cert.pem"), filepath.Join(certDir, "tls-client-key.pem"))
	if err != nil {
		t.Fatalf("failed to load the test client certificate: %v", err)
	}
	tlsConf := tlsClientConfig(&caCert)

	var hc *holdConn
	dialer := func(addr string) (net.Conn, error) {
		tc, err := net.DialTimeout("tcp", addr, time.Second)
		if err != nil {
			return nil, err
		}
		hc = &holdConn{Conn: tc}
		c := tls.Client(hc, tlsConf)
		if err := c.Handshake(); err != nil {
			tc.Close()
			return nil, err
		}
		return c, nil
	}
	tx := isclient.Connect(t, harness.Addr(t, 55154), "N5CAL", isclient.WithPasscode("-1"), isclient.WithDialer(dialer))

	// The server reads a limited number of records per client at a time,
	// SSL_READ_ROUNDS (64), so that a fast client can not keep a worker
	// to itself. The larger burst is a few times that and still fits in
	// OpenSSL's read buffer, so the socket is empty after the first read
	// and the server has to come back for the rest on its own.
	for _, n := range []int{16, 200} {
		t.Run(fmt.Sprintf("%d records", n), func(t *testing.T) {
			var sent []string
			for i := 0; i < n; i++ {
				sent = append(sent, fmt.Sprintf("N5CAL>APRS,TCPIP*,qAC,TESTING:>burst of %d packet %d", n, i))
			}

			// Each SendLine is one TLS record; Release sends them in one write.
			hc.Hold()
			for _, p := range sent {
				tx.Send(t, p)
			}
			if err := hc.Release(); err != nil {
				t.Fatalf("failed to send the burst: %v", err)
			}

			for i, want := range sent {
				got, err := rx.ReadPacket(3 * time.Second)
				if err != nil {
					t.Fatalf("received %d of %d packets of the burst, then: %v", i, n, err)
				}
				if got != want {
					t.Fatalf("packet %d: got %q, want %q", i, got, want)
				}
			}
		})
	}
}
