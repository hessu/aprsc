//go:build perf

package perf

import (
	"flag"
	"fmt"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

var (
	nclients   = flag.Int("nclients", 10, "number of full feed clients in the client scenarios")
	serverCPUs = flag.String("server-cpus", "", "CPUs to pin the servers to, as a list for taskset -c, so that the test process does not compete with them")
	clientCPUs = flag.String("client-cpus", "", "CPUs to pin the test process, the feeder and the clients, to")
)

// pin restricts a process and its threads to the given CPUs, when a list is
// given.
func pin(t *testing.T, cpus string, pid int) {
	t.Helper()
	if cpus == "" {
		return
	}
	if out, err := exec.Command("taskset", "-apc", cpus, strconv.Itoa(pid)).CombinedOutput(); err != nil {
		t.Fatalf("taskset -apc %s %d: %v: %s", cpus, pid, err, out)
	}
}

// clientServerConfig returns the configuration of the single server of the
// client scenarios: no peers, just the listeners. The IS2 full feed listener
// is only there when the clients need it, so that the plain scenario runs on
// a build without IS2 too.
func clientServerConfig(is2Listener bool) string {
	var b strings.Builder

	b.WriteString("ServerId PERFA\n")
	fmt.Fprintf(&b, "PassCode %s\n", isclient.Passcode("PERFA"))
	b.WriteString("MyEmail perf@example.com\n")
	b.WriteString("MyAdmin \"Performance test\"\n")
	b.WriteString("RunDir data\n")
	b.WriteString("UpstreamTimeout 10s\n")
	b.WriteString("ClientTimeout 48h\n")
	// room for the clients: the limits default to 500 in all and 200 per
	// listener
	limit := *nclients + 100
	fmt.Fprintf(&b, "Listen \"Full feed\" fullfeed tcp ::0 %d maxclients %d\n", aFullFeed, limit)
	fmt.Fprintf(&b, "Listen \"Igate\" igate tcp ::0 %d\n", aIgate)
	if is2Listener {
		fmt.Fprintf(&b, "Listen \"IS2 full feed\" is2fullfeed tcp ::0 %d maxclients %d\n", aIS2, limit)
	}
	fmt.Fprintf(&b, "HTTPStatus 127.0.0.1 %d\n", aStatus)
	b.WriteString("WorkerThreads 3\n")
	fmt.Fprintf(&b, "MaxClients %d\n", limit)
	fmt.Fprintf(&b, "FileLimit %d\n", limit+1000)

	return b.String()
}

// clientResult is the outcome of one client scenario.
type clientResult struct {
	name     string
	sent     int
	received int64 // over all the clients
	fed      time.Duration
	drain    time.Duration // from the end of the feed to the last packet received
	drained  bool          // every packet was received, so drain is a measurement
	u        usage
	lat      latencies
}

// TestClientCPU measures the CPU use of one server while a client feeds
// unique packets to it at a steady rate and a number of full feed clients,
// all plaintext TCP or all IS2 over TCP, receive every one of them. Each
// client type is a scenario of its own, with a server of its own. The
// clients are connected and the path checked before the feed starts.
func TestClientCPU(t *testing.T) {
	var results []clientResult

	for _, client := range strings.Split(*clients, ",") {
		name := client + "-clients"
		t.Run(name, func(t *testing.T) {
			if client == "is2" {
				harness.RequireIS2(t)
			}
			r := runClientScenario(t, client)
			r.name = name
			results = append(results, r)
		})
	}

	if len(results) == 0 {
		return
	}

	var b strings.Builder
	fmt.Fprintf(&b, "CPU use of the server over its life, %d packets/s fed for %v to %d full feed clients; cpu%% is of the feed time, us/pkt is per packet fed:\n", *rate, *duration, *nclients)
	fmt.Fprintf(&b, "%-14s %9s %9s %9s %7s %8s %8s %9s\n", "scenario", "user s", "sys s", "total s", "cpu%", "us/pkt", "drain s", "max RSS MB")
	for _, r := range results {
		total := r.u.user + r.u.sys
		fmt.Fprintf(&b, "%-14s %9.3f %9.3f %9.3f %7.1f %8.0f %8s %9.1f\n", r.name,
			r.u.user.Seconds(), r.u.sys.Seconds(), total.Seconds(),
			100*total.Seconds()/r.fed.Seconds(), float64(total.Microseconds())/float64(r.sent),
			drainColumn(r.drain, r.drained), float64(r.u.maxRSS)/(1024*1024))
	}
	fmt.Fprintf(&b, "\nLatency from the feeder to the clients, ms:\n")
	fmt.Fprintf(&b, "%-14s %8s %8s %8s %8s\n", "scenario", "mean", "median", "p99", "max")
	for _, r := range results {
		ms := func(d time.Duration) float64 { return float64(d.Microseconds()) / 1000 }
		fmt.Fprintf(&b, "%-14s %8.1f %8.1f %8.1f %8.1f\n", r.name, ms(r.lat.mean), ms(r.lat.median), ms(r.lat.p99), ms(r.lat.max))
	}
	t.Log(b.String())
}

func runClientScenario(t *testing.T, client string) clientResult {
	t.Helper()

	if client != "tcp" && client != "is2" {
		t.Fatalf("unknown client type %q", client)
	}

	s := harness.StartText(t, "perf-a", clientServerConfig(client == "is2"))
	pin(t, *serverCPUs, s.Pid())
	pin(t, *clientCPUs, os.Getpid())

	// the full feed clients, and the feeder on the igate port
	// the login names must fit a callsign: PX1 to PX1000
	rxs := make([]isclient.Receiver, *nclients)
	for i := range rxs {
		login := fmt.Sprintf("PX%d", i+1)
		if client == "is2" {
			rxs[i] = isclient.ConnectIS2(t, harness.Addr(t, aIS2), login)
		} else {
			rxs[i] = isclient.Connect(t, harness.Addr(t, aFullFeed), login)
		}
	}
	tx := isclient.Connect(t, harness.Addr(t, aIgate), feedLogin)

	// prove the path from the feeder to every client before the clock starts
	probe := "PT0>APRS,qAR," + feedLogin + ":>perf probe"
	tx.Send(t, probe)
	for i, rx := range rxs {
		if line, err := rx.ReadPacket(isclient.DefaultTimeout); err != nil || line != probe {
			t.Fatalf("client %d did not get the probe packet: %q, %v", i+1, line, err)
		}
	}

	// receive in the background, every client counting its packets; the
	// first client notes how long each took
	total := int(duration.Seconds() * float64(*rate))
	epoch := time.Now()
	samples := make([]time.Duration, 0, total)
	var received atomic.Int64
	stop := make(chan struct{})
	var wg sync.WaitGroup
	errs := make(chan error, len(rxs))
	for i, rx := range rxs {
		wg.Add(1)
		go func(i int, rx isclient.Receiver) {
			defer wg.Done()
			for {
				if line, err := rx.ReadPacket(time.Second); err == nil {
					if i == 0 {
						if sent, ok := sentAt(line); ok {
							samples = append(samples, time.Since(epoch)-sent)
						}
					}
					received.Add(1)
					continue
				} else if !isclient.IsTimeout(err) {
					errs <- fmt.Errorf("client %d: %w", i+1, err)
					return
				}
				select {
				case <-stop:
					return
				default:
				}
			}
		}(i, rx)
	}

	// feed at the rate, catching up after any hiccup so that the average
	// holds; a client which failed ends the feed, since the run is lost
	interval := time.Second / time.Duration(*rate)
	start := time.Now()
	for n := 0; n < total && len(errs) == 0; n++ {
		time.Sleep(time.Until(start.Add(time.Duration(n) * interval)))
		tx.Send(t, packet(n, epoch))
	}
	fed := time.Since(start)

	// let the last packets through: the IS2 client output is flushed in
	// batches, so it trails the feed a little
	want := int64(total) * int64(len(rxs))
	deadline := time.Now().Add(10 * time.Second)
	for received.Load() < want && len(errs) == 0 && time.Now().Before(deadline) {
		time.Sleep(20 * time.Millisecond)
	}
	drain := time.Since(start) - fed
	drained := received.Load() == want
	close(stop)
	wg.Wait()
	close(errs)
	for err := range errs {
		t.Errorf("a receiving client failed: %v", err)
	}

	s.Stop()

	got := received.Load()
	t.Logf("%s clients: fed %d packets in %.2f s (%.1f/s), the %d clients received %d of %d, %s",
		client, total, fed.Seconds(), float64(total)/fed.Seconds(), len(rxs), got, want, drainText(drain, drained))
	if got != want {
		t.Errorf("the clients received %d of %d packets", got, want)
	}

	r := clientResult{sent: total, received: got, fed: fed, drain: drain, drained: drained, u: usageOf(s.Rusage()), lat: summarize(samples)}
	t.Logf("latency from the feeder to the first client: %s", r.lat)
	t.Logf("server: %s", r.u.describe(fed, total))

	return r
}
