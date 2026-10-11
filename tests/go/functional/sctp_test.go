package functional

import (
	"encoding/json"
	"io"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// SCTP, only on aprsc: two servers, where the leaf connects to the hub with an
// SCTP uplink, and a few megabytes of traffic go through it. It is skipped if
// this build of aprsc has no SCTP support (the configuration is rejected).
// Port of t/69sctp.t.
func TestSCTP(t *testing.T) {
	harness.Parallel(t)
	harness.RequireAprsc(t)

	if out, err := harness.CheckConfig(t, "sctp-hub"); err != nil {
		if strings.Contains(out, "Unsupported protocol 'sctp'") {
			t.Skip("this aprsc build has no SCTP support")
		}
		t.Fatalf("configuration check failed: %v\n%s", err, out)
	}

	hub := harness.Start(t, "sctp-hub")
	leaf := harness.StartAdditional(t, "sctp-leaf")

	// wait for the leaf to connect to the hub
	httpClient := &http.Client{Timeout: 10 * time.Second}
	statusURL := "http://" + harness.AddrOn(t, "127.0.0.1", 55502) + "/status.json"
	var uplink map[string]any

	deadline := time.Now().Add(10 * time.Second)
	for uplink == nil && time.Now().Before(deadline) {
		uplink = leafUplink(httpClient, statusURL)
		if uplink == nil {
			time.Sleep(time.Second)
		}
	}
	if uplink == nil {
		t.Fatal("leaf server failed to establish an SCTP uplink with the hub")
	}

	const loginTx = "N0GAT"
	rx := isclient.Connect(t, harness.Addr(t, 55152), "N1GAT")
	tx := isclient.Connect(t, harness.Addr(t, 55583), loginTx)

	orderedLoad(t, tx, rx, loginTx, 4*1024*1024, 2*time.Second)

	hub.Stop()
	leaf.Stop()
}

// leafUplink returns the first uplink in the leaf's status.json at url, if
// it has logged in to the hub (as TESTING-1).
func leafUplink(c *http.Client, url string) map[string]any {
	res, err := c.Get(url)
	if err != nil {
		return nil
	}
	defer res.Body.Close()
	if res.StatusCode != 200 {
		return nil
	}

	body, err := io.ReadAll(res.Body)
	if err != nil {
		return nil
	}

	var j struct {
		Uplinks []map[string]any `json:"uplinks"`
	}
	if json.Unmarshal(body, &j) != nil || len(j.Uplinks) == 0 {
		return nil
	}

	if j.Uplinks[0]["username"] == "TESTING-1" {
		return j.Uplinks[0]
	}
	return nil
}
