//go:build perf

package perf

import (
	"bufio"
	"flag"
	"fmt"
	"math/rand"
	"net"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"testing"
	"time"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

var reloadSeed = flag.Int64("reload-seed", 0, "seed of the reload schedule of TestClientReload; 0 takes one from the clock, the test logs it")

const (
	aUpstream    = 10153 // the fake upstream the server uplinks to
	aChurnUDP    = 51342 // the UDP port the churning clients ask packets to
	upstreamRate = 50    // packets/s the fake upstream feeds down the uplink
	upstreamCall = "FAKEUP"
)

// reloadServerConfig is the client scenario configuration with the things
// the servers of the field reports have in addition: an uplink, and a UDP
// socket on the igate port, so that clients can ask for packets over UDP.
func reloadServerConfig() string {
	var b strings.Builder
	b.WriteString(clientServerConfig(false))
	fmt.Fprintf(&b, "Listen \"Igate UDP\" igate udp ::0 %d\n", aIgate)
	fmt.Fprintf(&b, "Uplink full1 full tcp 127.0.0.1 %d\n", aUpstream)
	return b.String()
}

// upstream is a fake upstream server: it lets the server under test log in,
// reads and drops what comes up the uplink, and feeds packets down it. It
// does not fail the test, as it runs in goroutines of its own: it counts.
type upstream struct {
	ln       net.Listener
	stop     chan struct{}
	wg       sync.WaitGroup
	conns    atomic.Int64 // uplink connections logged in
	received atomic.Int64 // lines read from the server
	sent     atomic.Int64 // packets fed to the server
}

func serveUpstream(t *testing.T, addr string) *upstream {
	t.Helper()

	ln, err := net.Listen("tcp", addr)
	if err != nil {
		t.Fatalf("failed to listen on %s for the fake upstream: %v", addr, err)
	}
	u := &upstream{ln: ln, stop: make(chan struct{})}
	t.Cleanup(u.close)

	u.wg.Add(1)
	go func() {
		defer u.wg.Done()
		for {
			conn, err := ln.Accept()
			if err != nil {
				return
			}
			u.wg.Add(1)
			go func() {
				defer u.wg.Done()
				u.serve(conn)
			}()
		}
	}()
	return u
}

func (u *upstream) close() {
	select {
	case <-u.stop:
		return
	default:
	}
	close(u.stop)
	u.ln.Close()
	u.wg.Wait()
}

func (u *upstream) serve(conn net.Conn) {
	defer conn.Close()
	r := bufio.NewReader(conn)

	conn.SetDeadline(time.Now().Add(5 * time.Second))
	if _, err := fmt.Fprintf(conn, "# IS_Fake 1.00\r\n"); err != nil {
		return
	}
	login, err := r.ReadString('\n')
	if err != nil {
		return
	}
	f := strings.Fields(login)
	if len(f) < 2 || f[0] != "user" {
		return
	}
	if _, err := fmt.Fprintf(conn, "# logresp %s verified, server %s\r\n", f[1], upstreamCall); err != nil {
		return
	}
	conn.SetDeadline(time.Time{})
	u.conns.Add(1)

	// read and drop whatever the server sends up, until it hangs up
	done := make(chan struct{})
	go func() {
		defer close(done)
		for {
			if _, err := r.ReadString('\n'); err != nil {
				return
			}
			u.received.Add(1)
		}
	}()

	// feed packets down at the rate until the connection goes
	interval := time.Second / upstreamRate
	tick := time.NewTicker(interval)
	defer tick.Stop()
	for n := 0; ; n++ {
		select {
		case <-u.stop:
			return
		case <-done:
			return
		case <-tick.C:
		}
		conn.SetWriteDeadline(time.Now().Add(2 * time.Second))
		if _, err := fmt.Fprintf(conn, "UP%d>APRS,qAR,UPGATE:>upstream %d\r\n", n%500, n); err != nil {
			return
		}
		u.sent.Add(1)
	}
}

// churnStats counts what the churning clients did.
type churnStats struct {
	dialed, dialFailed, loggedIn, refused, udp, reset atomic.Int64
}

// churn connects clients to the igate port one after another, each logging
// in, sending a few packets and leaving, as the clients of a busy server
// do. Half of them ask for the packets over UDP, the option the client
// which connected as a worker of a field server froze had used; some log in
// with a wrong passcode or a bad login line; half of them leave with a
// reset instead of a close. It does not fail the test: it counts.
func churn(addr string, udpPort int, rnd *rand.Rand, stop <-chan struct{}, st *churnStats) {
	for n := 0; ; n++ {
		gap := 20*time.Millisecond + time.Duration(rnd.Int63n(int64(130*time.Millisecond)))
		select {
		case <-stop:
			return
		case <-time.After(gap):
		}

		call := fmt.Sprintf("CH%d", n%1000)
		pass := isclient.Passcode(call)
		var login string
		switch rnd.Intn(6) {
		case 0:
			login = fmt.Sprintf("user %s pass %s vers Churn 1.0", call, pass)
		case 1:
			login = fmt.Sprintf("user %s pass %s vers Churn 1.0 filter m/50", call, pass)
		case 2:
			login = fmt.Sprintf("user %s pass %s vers Churn 1.0 UDP %d", call, pass, udpPort)
			st.udp.Add(1)
		case 3:
			login = fmt.Sprintf("user %s pass %s vers Churn 1.0 UDP %d filter r/60/25/100", call, pass, udpPort)
			st.udp.Add(1)
		case 4:
			login = fmt.Sprintf("user %s pass -1 vers Churn 1.0 UDP %d", call, udpPort)
			st.udp.Add(1)
		case 5:
			login = fmt.Sprintf("user %s pass %s vers Churn", call, pass) // rejected: no version
		}

		st.dialed.Add(1)
		conn, err := net.DialTimeout("tcp", addr, 2*time.Second)
		if err != nil {
			st.dialFailed.Add(1)
			continue
		}
		func() {
			defer conn.Close()
			r := bufio.NewReader(conn)
			conn.SetDeadline(time.Now().Add(3 * time.Second))
			if _, err := r.ReadString('\n'); err != nil { // the banner
				return
			}
			if _, err := fmt.Fprintf(conn, "%s\r\n", login); err != nil {
				return
			}
			reply, err := r.ReadString('\n')
			if err != nil {
				return
			}
			if !strings.HasPrefix(reply, "# logresp") {
				st.refused.Add(1)
				return
			}
			st.loggedIn.Add(1)

			// a few packets, with pauses, or nothing at all
			for i, k := 0, rnd.Intn(4); i < k; i++ {
				if _, err := fmt.Fprintf(conn, "%s>APRS,qAR,%s:>churn %d/%d\r\n", call, call, n, i); err != nil {
					return
				}
				select {
				case <-stop:
					return
				case <-time.After(time.Duration(rnd.Int63n(int64(100 * time.Millisecond)))):
				}
			}
			if rnd.Intn(2) == 0 {
				// leave with a reset
				if tc, ok := conn.(*net.TCPConn); ok {
					tc.SetLinger(0)
					st.reset.Add(1)
				}
			}
		}()
	}
}

// TestClientReload runs the plaintext client scenario of TestClientCPU on a
// server with an uplink, while the configuration is reloaded with SIGUSR1
// at irregular intervals, from a few milliseconds to a few seconds apart,
// half of the reloads with a changed configuration. A reload makes the main
// thread free and rebuild the configuration lists, and the accept and http
// threads apply them and rescan the clients against the ACLs, while the
// workers carry the load. Meanwhile a fake upstream feeds packets down the
// uplink, and clients come and go on the igate port, some asking for
// packets over UDP, some logging in badly, some leaving with a reset.
// Every packet of the feed must reach every client, the server must
// survive, and a probe must still reach every client after the last
// reload: a worker thread which stopped would leave its third of the
// clients without it.
//
// Run it with the load of the hang reports:
//
//	go test -tags perf -v -run TestClientReload ./perf/ -args -nclients 1000 -rate 200
func TestClientReload(t *testing.T) {
	harness.RequireAprsc(t)

	seed := *reloadSeed
	if seed == 0 {
		seed = time.Now().UnixNano()
	}
	t.Logf("reload schedule seed %d", seed)
	rnd := rand.New(rand.NewSource(seed))
	churnRnd := rand.New(rand.NewSource(seed + 1))

	const (
		readOK     = "New configuration read successfully"
		readFail   = "New configuration was not successfully read"
		deferred   = "Reconfiguration deferred"
		acceptOK   = "Accept thread ready"
		uplinkUp   = "Connection established"
		uplinkDown = "Uplink has been closed"
	)
	cfg := reloadServerConfig()

	// the fake upstream listens before the server starts, so that the
	// first uplink attempt succeeds
	up := serveUpstream(t, harness.AddrOn(t, "127.0.0.1", aUpstream))

	var mu sync.Mutex
	var signals, bursts, changed int
	var reads0, accepts0 int
	var srv *harness.Server
	var cs churnStats
	during := func(s *harness.Server, stop <-chan struct{}) {
		mu.Lock()
		srv = s
		reads0, accepts0 = s.LogCount(readOK), s.LogCount(acceptOK)
		mu.Unlock()

		var wg sync.WaitGroup
		wg.Add(1)
		go func() {
			defer wg.Done()
			churn(harness.Addr(t, aIgate), harness.Port(t, aChurnUDP), churnRnd, stop, &cs)
		}()
		defer wg.Wait()

		for n := 0; ; n++ {
			// the gap to the next reload: a third of them come in a
			// burst, within 30 ms of the previous one, a third
			// shortly after it, and the rest after the previous one
			// has had time to be applied
			var gap time.Duration
			switch r := rnd.Float64(); {
			case r < 0.35:
				gap = time.Duration(rnd.Int63n(int64(30 * time.Millisecond)))
			case r < 0.7:
				gap = 30*time.Millisecond + time.Duration(rnd.Int63n(int64(470*time.Millisecond)))
			default:
				gap = 500*time.Millisecond + time.Duration(rnd.Int63n(int64(2500*time.Millisecond)))
			}
			select {
			case <-stop:
				return
			case <-time.After(gap):
			}
			if !s.Running() {
				return
			}

			// half of the reloads change the configuration
			change := rnd.Intn(2) == 0
			if change {
				s.ReplaceConfig(strings.Replace(cfg, `MyAdmin "Performance test"`, fmt.Sprintf(`MyAdmin "Reload %d"`, n), 1))
			}
			s.Signal(syscall.SIGUSR1)

			mu.Lock()
			signals++
			if gap < 30*time.Millisecond {
				bursts++
			}
			if change {
				changed++
			}
			mu.Unlock()
		}
	}

	runClientLoad(t, "tcp", cfg, during)
	up.close()

	mu.Lock()
	defer mu.Unlock()
	if signals == 0 {
		t.Fatal("no reloads were signalled during the load")
	}
	// SIGUSR1s which arrive within one round of the main loop merge into
	// one read, so the reads are fewer than the signals
	reads := srv.LogCount(readOK) - reads0
	t.Logf("%d SIGUSR1 sent, %d of them within 30 ms of the previous, %d with a changed configuration; %d configurations read, %d deferred, %d accept thread restarts",
		signals, bursts, changed, reads, srv.LogCount(deferred), srv.LogCount(acceptOK)-accepts0)
	t.Logf("uplink: %d connections logged in to the fake upstream, %d established and %d closed in the server's log; the upstream fed %d packets down and read %d lines coming up",
		up.conns.Load(), srv.LogCount(uplinkUp), srv.LogCount(uplinkDown), up.sent.Load(), up.received.Load())
	t.Logf("churn: %d clients dialed, %d could not connect, %d logged in, %d were refused, %d asked for UDP, %d left with a reset",
		cs.dialed.Load(), cs.dialFailed.Load(), cs.loggedIn.Load(), cs.refused.Load(), cs.udp.Load(), cs.reset.Load())
	if reads == 0 {
		t.Errorf("none of the %d reloads was read", signals)
	}
	if n := srv.LogCount(readFail); n != 0 {
		t.Errorf("%d configuration reads failed", n)
	}
	if up.conns.Load() == 0 {
		t.Error("the server never logged in to the fake upstream")
	}
	if cs.loggedIn.Load() == 0 {
		t.Error("none of the churning clients could log in")
	}
	if n := cs.dialFailed.Load(); n > cs.dialed.Load()/10 {
		t.Errorf("%d of %d churning clients could not connect", n, cs.dialed.Load())
	}
}
