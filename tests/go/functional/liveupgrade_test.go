package functional

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"syscall"
	"testing"
	"time"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// statusJSON is the part of the server's status.json the tests look at.
type statusJSON struct {
	Server    map[string]any   `json:"server"`
	Totals    map[string]any   `json:"totals"`
	Dupecheck map[string]any   `json:"dupecheck"`
	Listeners []map[string]any `json:"listeners"`
	Peers     []map[string]any `json:"peers"`
	Clients   []map[string]any `json:"clients"`
}

// clientLinks returns the link type of each client in the status, by
// username: "is2", or "" for plaintext.
func (j *statusJSON) clientLinks() map[string]string {
	links := make(map[string]string)
	for _, c := range j.Clients {
		name, _ := c["username"].(string)
		link, _ := c["link"].(string)
		links[name] = link
	}
	return links
}

// peerLinks returns the link type of each peer in the status, by username:
// "is2", or "" for plain UDP peering.
func (j *statusJSON) peerLinks() map[string]string {
	links := make(map[string]string)
	for _, p := range j.Peers {
		name, _ := p["username"].(string)
		link, _ := p["link"].(string)
		links[name] = link
	}
	return links
}

// getStatus fetches and decodes the status page of the server under test.
func getStatus(t *testing.T) *statusJSON {
	t.Helper()

	client := &http.Client{Timeout: 10 * time.Second}
	res, err := client.Get("http://" + harness.AddrOn(t, "127.0.0.1", 55501) + "/status.json")
	if err != nil {
		t.Fatalf("HTTP GET of status.json failed: %v", err)
	}
	defer res.Body.Close()

	if res.StatusCode != 200 {
		t.Fatalf("HTTP GET of status.json returned %s", res.Status)
	}

	body, err := io.ReadAll(res.Body)
	if err != nil {
		t.Fatalf("reading status.json failed: %v", err)
	}

	var j statusJSON
	if err := json.Unmarshal(body, &j); err != nil {
		t.Fatalf("JSON decoding of status.json failed: %v", err)
	}

	return &j
}

// Live upgrade, only on aprsc: SIGUSR2 makes the server run the new binary in
// place, handing the client connections over. Port of t/70live-upgrade.t.
func TestLiveUpgrade(t *testing.T) {
	harness.Parallel(t)
	harness.RequireIS2(t) // the test configuration has IS2 ports, to see them survive too

	s := harness.Start(t, "is2-basic")

	// the status page without any traffic
	if getStatus(t).Server == nil {
		t.Error("pre-upgrade status.json does not define 'server'")
	}

	// connect IS clients: one transmitting, a full feed, and an IS2 client
	const login = "N5CAL-10"
	tx := isclient.Connect(t, harness.Addr(t, 55580), login)
	rx := isclient.Connect(t, harness.Addr(t, 55152), "N5CAL-2")
	is2 := isclient.ConnectIS2(t, harness.Addr(t, 56152), "N5CAL-4")
	if err := is2.Ping(); err != nil {
		t.Errorf("failed to ping the IS2 server: %v", err)
	}

	// Send a packet, a duplicate, and a third dummy packet. The unique packet
	// and the helper will appear on the IS2 full feed too.
	uniq := "SRC>DST,qAR," + login + ":foo1"
	helper := "SRC>DST,qAR," + login + ":dummy1"
	isclient.TxRx(t, tx, rx, uniq, uniq)
	isclient.ExpectDropLiteral(t, tx, rx, "SRC>DST,DIGI1*,qAR,"+login+":foo1", helper)
	expectIS2 := func(want ...string) {
		t.Helper()
		for _, w := range want {
			if got, err := is2.ReadPacket(isclient.DefaultTimeout); err != nil || got != w {
				t.Errorf("IS2 full feed: got %q (%v), want %q", got, err, w)
			}
		}
	}
	expectIS2(uniq, helper)

	// Delete the old live upgrade status file, ignoring errors if it does not
	// happen to exist yet. The new instance writes it when it is up.
	upgraded := filepath.Join(s.RunDir(), "liveupgrade.json.old")
	os.Remove(upgraded)

	s.Signal(syscall.SIGUSR2)

	// It takes some time for aprsc to shut down and reload, some 0.5 s
	// usually, but a virtual machine might be slow.
	const maxWait = 10 * time.Second
	deadline := time.Now().Add(maxWait)
	for time.Now().Before(deadline) {
		if _, err := os.Stat(upgraded); err == nil {
			break
		}
		time.Sleep(100 * time.Millisecond)
	}
	if _, err := os.Stat(upgraded); err != nil {
		t.Fatalf("live upgrade not done, timed out in %v, %s not present", maxWait, upgraded)
	}

	// the new instance greets the IS2 client with a server signature again
	if err := is2.WaitSignature(isclient.DefaultTimeout); err != nil {
		t.Errorf("IS2 server signature not received after the upgrade: %v", err)
	}
	if err := is2.Ping(); err != nil {
		t.Errorf("failed to ping the IS2 server after the upgrade: %v", err)
	}

	// Do the same test again: the dupecheck cache has been lost in the upgrade.
	helper = "SRC>DST,qAR," + login + ":dummy2"
	isclient.TxRx(t, tx, rx, uniq, uniq)
	isclient.ExpectDropLiteral(t, tx, rx, uniq, helper)
	expectIS2(uniq, helper)

	// The new instance's HTTP thread binds the status port on its own; wait for
	// its second "ready" line in the log before asking for the status page.
	s.WaitLog("HTTP thread ready.", 2, isclient.DefaultTimeout)

	// The worker threads take some time to accumulate statistics. The counters
	// must include only the packets sent after the reload: 2 uniques, 1 dupe.
	var dupecheck map[string]any
	deadline = time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		if dupecheck = getStatus(t).Dupecheck; dupecheck != nil && dupecheck["uniques_out"] == float64(2) {
			break
		}
		time.Sleep(250 * time.Millisecond)
	}

	if dupecheck == nil {
		t.Fatal("post-upgrade status.json does not define 'dupecheck'")
	}
	if got := fmt.Sprint(dupecheck["uniques_out"]); got != "2" {
		t.Errorf("post-upgrade uniques_out is %s, want 2", got)
	}
	if got := fmt.Sprint(dupecheck["dupes_dropped"]); got != "1" {
		t.Errorf("post-upgrade dupes_dropped is %s, want 1", got)
	}

	s.Stop()
}
