//go:build perf

// Package perf has performance scenarios for aprsc. They measure rather than
// check, and take their time, so they are not part of the test suite: run
// them with "make perftest" in tests/, or with go test -tags perf -v ./perf/
// in tests/go/, and pass flags after -args to vary the load.
package perf

import (
	"flag"
	"fmt"
	"runtime"
	"strings"
	"sync/atomic"
	"syscall"
	"testing"
	"time"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

var (
	duration = flag.Duration("duration", 30*time.Second, "how long to feed packets")
	rate     = flag.Int("rate", 200, "packets to feed per second")
	peers    = flag.String("peers", "udp,is2", "peer link types to measure: udp, is2")
	clients  = flag.String("clients", "tcp,is2", "full feed client types to measure: tcp, is2")
)

// The port numbers of the two servers, as keys of the harness port table: A
// takes the feed and passes it to B over the peer link, B feeds the client.
const (
	aFullFeed, aIgate, aIS2, aStatus, aPeer = 55152, 55580, 56152, 55501, 16404
	bFullFeed, bIgate, bIS2, bStatus, bPeer = 55153, 55581, 56153, 55502, 16405

	feedLogin = "PERFTX"
	rxLogin   = "PERFRX"
)

// serverConfig returns the configuration of one of the servers: its id and
// ports, and the other server as its UDP peer. With keys the peer link
// upgrades to IS2, without them it stays on plain UDP. The IS2 full feed
// listener is only there when the client needs it, so that the plain
// scenarios run on a build without IS2 too.
func serverConfig(id string, fullFeed, igate, is2, status, peer int, peerID string, peerPort int, keys, is2Listener bool) string {
	var b strings.Builder

	fmt.Fprintf(&b, "ServerId %s\n", id)
	fmt.Fprintf(&b, "PassCode %s\n", isclient.Passcode(id))
	b.WriteString("MyEmail perf@example.com\n")
	b.WriteString("MyAdmin \"Performance test\"\n")
	b.WriteString("RunDir data\n")
	b.WriteString("UpstreamTimeout 10s\n")
	b.WriteString("ClientTimeout 48h\n")
	fmt.Fprintf(&b, "Listen \"Full feed\" fullfeed tcp ::0 %d\n", fullFeed)
	fmt.Fprintf(&b, "Listen \"Igate\" igate tcp ::0 %d\n", igate)
	if is2Listener {
		fmt.Fprintf(&b, "Listen \"IS2 full feed\" is2fullfeed tcp ::0 %d\n", is2)
	}
	fmt.Fprintf(&b, "PeerGroup PERF udp 127.0.0.1:%d %s 127.0.0.1:%d\n", peer, peerID, peerPort)
	if keys {
		b.WriteString("PeerGroupKey 1 perfkey\n")
		b.WriteString("PeerGroupKeyTransmit 1\n")
	}
	fmt.Fprintf(&b, "HTTPStatus 127.0.0.1 %d\n", status)
	b.WriteString("WorkerThreads 3\n")
	b.WriteString("FileLimit 10000\n")

	return b.String()
}

// packet returns the n:th packet of the feed. Each one is unique, so that the
// duplicate filter passes all of them.
func packet(n int) string {
	return fmt.Sprintf("PT%d>APRS,qAR,%s:!6028.51N/02505.68E# perf packet %d blaa blaa blaa END", n%1000+10, feedLogin, n)
}

// usage is what a server process used over its life.
type usage struct {
	user, sys   time.Duration
	maxRSS      int64 // bytes
	vcsw, ivcsw int64 // voluntary and involuntary context switches
}

func usageOf(ru *syscall.Rusage) usage {
	u := usage{
		user:   time.Duration(ru.Utime.Sec)*time.Second + time.Duration(ru.Utime.Usec)*time.Microsecond,
		sys:    time.Duration(ru.Stime.Sec)*time.Second + time.Duration(ru.Stime.Usec)*time.Microsecond,
		maxRSS: int64(ru.Maxrss),
		vcsw:   int64(ru.Nvcsw),
		ivcsw:  int64(ru.Nivcsw),
	}
	if runtime.GOOS != "darwin" {
		u.maxRSS *= 1024 // kilobytes, except on macOS, where it is bytes
	}
	return u
}

// describe formats the usage against the length of the feed and the number
// of packets which went through the server. The usage covers the whole life
// of the server, but outside the feed it had next to nothing to do.
func (u usage) describe(fed time.Duration, packets int) string {
	total := u.user + u.sys
	return fmt.Sprintf("user %.3f s, sys %.3f s, total %.3f s: %.1f%% of the %.1f s feed, %.0f us/packet; max RSS %.1f MB; context switches %d voluntary, %d involuntary",
		u.user.Seconds(), u.sys.Seconds(), total.Seconds(),
		100*total.Seconds()/fed.Seconds(), fed.Seconds(),
		float64(total.Microseconds())/float64(packets),
		float64(u.maxRSS)/(1024*1024), u.vcsw, u.ivcsw)
}

// result is the outcome of one scenario.
type result struct {
	name           string
	sent, received int
	fed            time.Duration // the length of the feed
	drain          time.Duration // from the end of the feed to the last packet received
	a, b           usage
}

// TestCorepeerCPU measures the CPU use of two servers while a client feeds
// unique packets to server A at a steady rate, A passes them on to server B
// over a UDP core peer link, and a full feed client of B receives them all.
// The peer link is plain UDP or IS2, and the client a plaintext TCP or an
// IS2 one; each combination is a scenario of its own, with its own servers.
// The clients are connected and the path checked before the feed starts.
func TestCorepeerCPU(t *testing.T) {
	var results []result

	for _, peer := range strings.Split(*peers, ",") {
		for _, client := range strings.Split(*clients, ",") {
			name := peer + "-peer/" + client + "-client"
			t.Run(name, func(t *testing.T) {
				if peer == "is2" || client == "is2" {
					harness.RequireIS2(t)
				}
				r := runScenario(t, peer, client)
				r.name = name
				results = append(results, r)
			})
		}
	}

	if len(results) == 0 {
		return
	}

	var b strings.Builder
	fmt.Fprintf(&b, "CPU use of the servers over their life, %d packets/s fed for %v; cpu%% is of the feed time:\n", *rate, *duration)
	fmt.Fprintf(&b, "%-22s %-6s %9s %9s %9s %7s %8s %8s\n", "scenario", "server", "user s", "sys s", "total s", "cpu%", "us/pkt", "drain s")
	for _, r := range results {
		for _, s := range []struct {
			name string
			u    usage
		}{{"A", r.a}, {"B", r.b}} {
			total := s.u.user + s.u.sys
			fmt.Fprintf(&b, "%-22s %-6s %9.3f %9.3f %9.3f %7.1f %8.0f %8.2f\n", r.name, s.name,
				s.u.user.Seconds(), s.u.sys.Seconds(), total.Seconds(),
				100*total.Seconds()/r.fed.Seconds(), float64(total.Microseconds())/float64(r.sent),
				r.drain.Seconds())
		}
	}
	t.Log(b.String())
}

func runScenario(t *testing.T, peer, client string) result {
	t.Helper()

	if peer != "udp" && peer != "is2" {
		t.Fatalf("unknown peer link type %q", peer)
	}
	if client != "tcp" && client != "is2" {
		t.Fatalf("unknown client type %q", client)
	}
	keys := peer == "is2"

	// B first, so that A finds it there when it proposes IS2 peering
	b := harness.StartText(t, "perf-b", serverConfig("PERFB", bFullFeed, bIgate, bIS2, bStatus, bPeer, "PERFA", aPeer, keys, client == "is2"))
	a := harness.StartText(t, "perf-a", serverConfig("PERFA", aFullFeed, aIgate, aIS2, aStatus, aPeer, "PERFB", bPeer, keys, false))

	// the clients: the receiver on B's full feed, the feeder on A's igate port
	var rx isclient.Receiver
	if client == "is2" {
		rx = isclient.ConnectIS2(t, harness.Addr(t, bIS2), rxLogin)
	} else {
		rx = isclient.Connect(t, harness.Addr(t, bFullFeed), rxLogin)
	}
	tx := isclient.Connect(t, harness.Addr(t, aIgate), feedLogin)

	// the peer link must be in the mode under test before the feed starts
	const upgraded = "IS2 UDP peer mode enabled"
	if keys {
		a.WaitLog(upgraded, 1, 30*time.Second)
		b.WaitLog(upgraded, 1, 30*time.Second)
	}

	// prove the path from the feeder to the receiver before the clock starts
	probe := "PT0>APRS,qAR," + feedLogin + ":>perf probe"
	isclient.TxRx(t, tx, rx, probe, probe)
	if t.Failed() {
		t.FailNow()
	}

	// receive in the background, counting
	var received atomic.Int64
	stop := make(chan struct{})
	rxDone := make(chan error, 1)
	go func() {
		for {
			if _, err := rx.ReadPacket(time.Second); err == nil {
				received.Add(1)
				continue
			} else if !isclient.IsTimeout(err) {
				rxDone <- err
				return
			}
			select {
			case <-stop:
				rxDone <- nil
				return
			default:
			}
		}
	}()

	// feed at the rate, catching up after any hiccup so that the average
	// holds
	total := int(duration.Seconds() * float64(*rate))
	interval := time.Second / time.Duration(*rate)
	start := time.Now()
	for n := 0; n < total; n++ {
		time.Sleep(time.Until(start.Add(time.Duration(n) * interval)))
		tx.Send(t, packet(n))
	}
	fed := time.Since(start)

	// Let the last packets through: the servers flush their peer and IS2
	// client output in batches, so they trail the feed a little.
	deadline := time.Now().Add(10 * time.Second)
	for received.Load() < int64(total) && time.Now().Before(deadline) {
		time.Sleep(20 * time.Millisecond)
	}
	drain := time.Since(start) - fed
	close(stop)
	if err := <-rxDone; err != nil {
		t.Errorf("the receiving client failed: %v", err)
	}

	a.Stop()
	b.Stop()

	got := int(received.Load())
	t.Logf("%s peer link, %s client: fed %d packets in %.2f s (%.1f/s), received %d, the last %.2f s after the feed ended",
		peer, client, total, fed.Seconds(), float64(total)/fed.Seconds(), got, drain.Seconds())
	if got != total {
		t.Errorf("the client received %d of %d packets", got, total)
	}
	if peer == "udp" {
		if a.LogCount(upgraded)+b.LogCount(upgraded) > 0 {
			t.Errorf("the peer link was upgraded to IS2 in the plain UDP scenario")
		}
	}

	r := result{sent: total, received: got, fed: fed, drain: drain, a: usageOf(a.Rusage()), b: usageOf(b.Rusage())}
	t.Logf("server A (feed in, peer out): %s", r.a.describe(fed, total))
	t.Logf("server B (peer in, client out): %s", r.b.describe(fed, total))

	return r
}
