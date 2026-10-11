package functional

import (
	"compress/gzip"
	"encoding/json"
	"io"
	"net/http"
	"testing"
	"time"

	"github.com/hessu/aprsc/tests/go/internal/harness"
)

// HTTP status service, only on aprsc. Port of t/66http-status.t.
func TestHTTPStatus(t *testing.T) {
	harness.RequireAprsc(t)

	s := harness.Start(t, "basic")

	// Compression is requested explicitly, and decoded below.
	client := &http.Client{
		Timeout:   10 * time.Second,
		Transport: &http.Transport{DisableCompression: true},
		CheckRedirect: func(*http.Request, []*http.Request) error {
			return http.ErrUseLastResponse
		},
	}

	// get fetches a URL, optionally accepting compression, and returns the
	// decoded body.
	get := func(t *testing.T, url string, compressed bool) []byte {
		t.Helper()

		req, err := http.NewRequest("GET", url, nil)
		if err != nil {
			t.Fatal(err)
		}
		req.Header.Set("User-Agent", "httpaprstester/1.0")
		if compressed {
			req.Header.Set("Accept-Encoding", "gzip")
		}

		res, err := client.Do(req)
		if err != nil {
			t.Fatalf("GET %s failed: %v", url, err)
		}
		defer res.Body.Close()

		if res.StatusCode != 200 {
			t.Fatalf("GET %s returned %s", url, res.Status)
		}

		var body io.Reader = res.Body
		if res.Header.Get("Content-Encoding") == "gzip" {
			zr, err := gzip.NewReader(res.Body)
			if err != nil {
				t.Fatalf("GET %s: bad gzip response: %v", url, err)
			}
			defer zr.Close()
			body = zr
		}

		b, err := io.ReadAll(body)
		if err != nil {
			t.Fatalf("GET %s: reading the body failed: %v", url, err)
		}
		return b
	}

	// getJSON fetches and decodes a JSON document, and checks that it defines
	// the given top-level keys.
	getJSON := func(t *testing.T, url string, compressed bool, keys ...string) {
		t.Helper()

		var j map[string]any
		if err := json.Unmarshal(get(t, url, compressed), &j); err != nil {
			t.Fatalf("JSON decoding of %s failed: %v", url, err)
		}
		for _, k := range keys {
			if _, ok := j[k]; !ok {
				t.Errorf("%s does not define %q", url, k)
			}
		}
	}

	const base = "http://127.0.0.1:55501"

	for _, compressed := range []bool{false, true} {
		name := "plain"
		if compressed {
			name = "compressed"
		}

		t.Run(name, func(t *testing.T) {
			get(t, base+"/", compressed)
			getJSON(t, base+"/status.json", compressed, "server")
			getJSON(t, base+"/counterdata?totals.tcp_bytes_rx", compressed, "interval", "values")
		})
	}

	s.Stop()
}
