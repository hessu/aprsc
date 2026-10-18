package functional

import (
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"syscall"
	"testing"
	"time"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// Two reloads back to back, with traffic flowing: the second SIGUSR1 goes
// out as soon as the first configuration has been read, while the accept and
// http threads may still be applying it. Reading a configuration frees the
// listener, peer and http configuration lists those threads walk, so the
// main thread defers the second read until they have caught up. The uplink
// thread takes its new configuration from where read_config() left it, and
// a read while the previous one is still waiting there discards the new
// uplink configuration, so the read is deferred for it too. Both reloads
// must be read and applied, and the server must keep working.
func TestReloadBackToBack(t *testing.T) {
	harness.Parallel(t)
	harness.RequireAprsc(t)

	src, err := os.ReadFile(filepath.Join(harness.TestsDir(), "cfg-aprsc", "basic"))
	if err != nil {
		t.Fatal(err)
	}

	s := harness.Start(t, "basic")
	client := isclient.Connect(t, harness.Addr(t, 55152), "N5CAL-1")
	udp := isclient.ListenUDPPeer(t, harness.AddrOn(t, "127.0.0.1", 16405), harness.AddrOn(t, "127.0.0.1", 16404))

	// packets both ways, from the client to the UDP peer and back, for the
	// whole time; the readers drain what comes out
	stop := make(chan struct{})
	var wg sync.WaitGroup
	wg.Add(4)
	go func() {
		defer wg.Done()
		for i := 0; ; i++ {
			select {
			case <-stop:
				return
			default:
			}
			if err := client.SendLine(fmt.Sprintf("N5CAL-1>APRS:>from the client %d", i)); err != nil {
				return
			}
			time.Sleep(time.Millisecond)
		}
	}()
	go func() {
		defer wg.Done()
		for i := 0; ; i++ {
			select {
			case <-stop:
				return
			default:
			}
			if err := udp.SendLine(fmt.Sprintf("P%d>APRS,qAR,PEER1:>from the peer %d", i%50, i)); err != nil {
				return
			}
			time.Sleep(time.Millisecond)
		}
	}()
	go func() {
		defer wg.Done()
		for {
			select {
			case <-stop:
				return
			default:
			}
			client.ReadPacket(200 * time.Millisecond)
		}
	}()
	go func() {
		defer wg.Done()
		for {
			select {
			case <-stop:
				return
			default:
			}
			udp.ReadPacket(200 * time.Millisecond)
		}
	}()

	const readOK = "New configuration read successfully"
	const acceptOK = "Accept thread ready"
	const httpOK = "HTTP thread ready"
	reads, accepts, https := s.LogCount(readOK), s.LogCount(acceptOK), s.LogCount(httpOK)
	if accepts != 1 || https != 1 {
		t.Fatalf("the server started with %d %q and %d %q lines in its log, want 1 of each", accepts, acceptOK, https, httpOK)
	}

	const rounds = 4
	for i := 0; i < rounds; i++ {
		s.ReplaceConfig(string(src))
		s.Signal(syscall.SIGUSR1)
		s.WaitLog(readOK, reads+1, 10*time.Second)
		// the previous configuration is being applied now
		s.Signal(syscall.SIGUSR1)
		s.WaitLog(readOK, reads+2, 10*time.Second)
		reads += 2
	}

	// each configuration read was applied by both threads, and the uplink
	// thread took each new uplink configuration
	s.WaitLog(acceptOK, accepts+2*rounds, 10*time.Second)
	s.WaitLog(httpOK, https+2*rounds, 10*time.Second)
	if n := s.LogCount("New uplink config discarded"); n != 0 {
		t.Errorf("the uplink configuration of a reload was discarded %d times", n)
	}
	t.Logf("%d reloads, deferred %d times", 2*rounds, s.LogCount("Reconfiguration deferred"))

	close(stop)
	wg.Wait()

	// drain what the load left behind, and check that packets still go
	// through both ways
	for {
		if _, err := client.ReadPacket(300 * time.Millisecond); err != nil {
			break
		}
	}
	for {
		if _, err := udp.ReadPacket(300 * time.Millisecond); err != nil {
			break
		}
	}
	const fromClient = "SRC>DST,qAR,IGATE:from the client after the reloads"
	isclient.TxRx(t, client, udp, fromClient, fromClient)
	const fromPeer = "SRC>DST,qAR,IGATE:from the peer after the reloads"
	isclient.TxRx(t, udp, client, fromPeer, fromPeer)

	s.Stop()
}
