//go:build perf

package perf

import (
	"encoding/json"
	"flag"
	"fmt"
	"net/http"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

var floodRate = flag.Int("flood-rate", 20000, "packets/s of unique stations TestHistoryDBFlood feeds; the dupecheck arena must hold 40 s of them")

// fetchStatus returns the decoded status.json of a server.
func fetchStatus(t *testing.T, statusPort int) map[string]any {
	t.Helper()

	client := &http.Client{Timeout: 10 * time.Second}
	res, err := client.Get("http://" + harness.AddrOn(t, "127.0.0.1", statusPort) + "/status.json")
	if err != nil {
		t.Fatalf("HTTP GET of status.json failed: %v", err)
	}
	defer res.Body.Close()
	if res.StatusCode != 200 {
		t.Fatalf("HTTP GET of status.json returned %s", res.Status)
	}

	var j map[string]any
	if err := json.NewDecoder(res.Body).Decode(&j); err != nil {
		t.Fatalf("JSON decoding of status.json failed: %v", err)
	}
	return j
}

// statusNumber returns a number from a section of the status.
func statusNumber(j map[string]any, section, key string) float64 {
	s, _ := j[section].(map[string]any)
	v, _ := s[key].(float64)
	return v
}

// TestHistoryDBFlood feeds a server position packets from unique stations
// until the position cache (historydb) is 90% full, and checks that the
// server then shortens the time it keeps the entries from the configured
// 24 h to 1 h, so that the cache recovers from a flood of random callsigns.
// A probe packet must still pass afterwards. The scenario takes the server
// to about 600 MB and a couple of minutes, which is why it is not in the
// test suite: the cache holds about 2.4M entries, and -flood-rate paces the
// feed so that the dupecheck arena, which also gets an entry per packet for
// 30 to 40 s, does not fill first.
func TestHistoryDBFlood(t *testing.T) {
	harness.RequireAprsc(t)

	s := harness.StartText(t, "perf-a", clientServerConfig(false))
	pin(t, *serverCPUs, s.Pid())
	pin(t, *clientCPUs, os.Getpid())

	rx := isclient.Connect(t, harness.Addr(t, aFullFeed), "PX1")
	tx := isclient.Connect(t, harness.Addr(t, aIgate), feedLogin)

	probe := "PT0>APRS,qAR," + feedLogin + ":>flood probe"
	isclient.TxRx(t, tx, rx, probe, probe)

	// the capacity of the cache, from the arena's block limit
	j := fetchStatus(t, aStatus)
	capacity := statusNumber(j, "memory", "historydb_blocks_max") *
		float64(int(statusNumber(j, "memory", "historydb_block_size"))/int(statusNumber(j, "memory", "historydb_cell_size_aligned")))
	storetime := statusNumber(j, "historydb", "storetime")
	if capacity < 1000 || storetime != 24*60*60 {
		t.Fatalf("historydb capacity %v entries, storetime %v s, want a limited cache with the default 24 h", capacity, storetime)
	}
	t.Logf("historydb capacity %.0f entries, feeding unique stations at %d packets/s", capacity, *floodRate)

	// feed unique stations, FLA00000 to FLZ99999, in bursts every 100 ms,
	// until the server reports the short storetime; the full feed client
	// reads and drops them in the background until told to stop
	stop := make(chan struct{})
	drained := make(chan struct{})
	go func() {
		defer close(drained)
		for {
			select {
			case <-stop:
				return
			default:
			}
			if _, err := rx.ReadPacket(time.Second); err != nil && !isclient.IsTimeout(err) {
				return
			}
		}
	}()

	const burstInterval = 100 * time.Millisecond
	burst := *floodRate / 10
	sent := 0
	start := time.Now()
	deadline := start.Add(10 * time.Minute)
	var b strings.Builder
	var lastStatus time.Time
	for {
		b.Reset()
		for i := 0; i < burst; i++ {
			fmt.Fprintf(&b, "FL%c%05d>APRS,qAR,%s:!6028.51N/02505.68E#\r\n", 'A'+sent/100000, sent%100000, feedLogin)
			sent++
		}
		if err := tx.SendRaw([]byte(b.String())); err != nil {
			t.Fatalf("failed to send after %d packets: %v", sent, err)
		}

		if time.Since(lastStatus) >= time.Second {
			lastStatus = time.Now()
			j = fetchStatus(t, aStatus)
			used := statusNumber(j, "memory", "historydb_cells_used")
			if statusNumber(j, "historydb", "storetime") == 3600 {
				t.Logf("short storetime after %d packets sent, %.0f entries in use (%.1f%% of the capacity), %v",
					sent, used, 100*used/capacity, time.Since(start).Round(time.Second))
				break
			}
			if used < 0.9*capacity && float64(sent) > 0.95*capacity {
				t.Fatalf("%d packets sent but only %.0f entries in the cache: the dupecheck arena is probably full, lower -flood-rate", sent, used)
			}
		}
		if float64(sent) >= capacity {
			t.Fatalf("%d packets sent, the whole capacity, and the storetime is still %v s", sent, statusNumber(j, "historydb", "storetime"))
		}
		if time.Now().After(deadline) {
			t.Fatalf("storetime still %v s after %d packets and %v", statusNumber(j, "historydb", "storetime"), sent, time.Since(start))
		}
		time.Sleep(burstInterval)
	}

	mb := func(key string) float64 { return statusNumber(j, "memory", key) / (1024 * 1024) }
	t.Logf("arenas allocated: historydb %.0f MB, dupecheck %.0f MB, pbuf small %.0f MB, medium %.0f MB, large %.0f MB",
		mb("historydb_allocated_bytes"), mb("dupecheck_allocated_bytes"),
		mb("pbuf_small_allocated_bytes"), mb("pbuf_medium_allocated_bytes"), mb("pbuf_large_allocated_bytes"))

	// the server still works; the drainer must be out of the way first,
	// since the connection's reader is not safe for concurrent use
	close(stop)
	<-drained
	probe = "PT1>APRS,qAR," + feedLogin + ":>flood probe after"
	tx.Send(t, probe)
	deadline = time.Now().Add(30 * time.Second)
	for {
		line, err := rx.ReadPacket(time.Until(deadline))
		if err != nil {
			t.Fatalf("the probe packet did not arrive after the flood: %v", err)
		}
		if line == probe {
			break
		}
	}

	s.Stop()
}
