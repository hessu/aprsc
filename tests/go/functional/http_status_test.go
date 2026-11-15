package functional

import (
	"bufio"
	"compress/gzip"
	"encoding/json"
	"io"
	"net"
	"net/http"
	"testing"
	"time"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// HTTP status service, only on aprsc. Port of t/66http-status.t.
func TestHTTPStatus(t *testing.T) {
	harness.Parallel(t)
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

	base := "http://" + harness.AddrOn(t, "127.0.0.1", 55501)

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

	// statusCode fetches a URL and returns the HTTP status code.
	statusCode := func(t *testing.T, url string) int {
		t.Helper()

		res, err := client.Get(url)
		if err != nil {
			t.Fatalf("GET %s failed: %v", url, err)
		}
		res.Body.Close()
		return res.StatusCode
	}

	// The translated strings of the web UI are served by language. A
	// request without a lang parameter, or with an unknown language, gets
	// a 404.
	t.Run("strings", func(t *testing.T) {
		getJSON(t, base+"/strings?lang=en", false, "APRSC_STATUS_TITLE")
		for _, q := range []string{"?lang=xx", "?x=1", "?", ""} {
			if code := statusCode(t, base+"/strings"+q); code != 404 {
				t.Errorf("GET /strings%s returned %d, want 404", q, code)
			}
		}
		getJSON(t, base+"/status.json", false, "server")
	})

	// the IS2 capability flags are there, and none of the listeners of
	// this configuration accept IS2 clients
	j := getStatus(t)
	if _, ok := j.Server["is2"].(bool); !ok {
		t.Errorf("server is2 is %v, want a boolean", j.Server["is2"])
	}
	for _, l := range j.Listeners {
		if is2, ok := l["is2"].(bool); !ok || is2 {
			t.Errorf("listener %q is2 is %v, want false", l["name"], l["is2"])
		}
	}

	// Each cellmalloc arena reports its own block limit. The pbuf and
	// dupecheck arenas have limits of their own, which together bound the
	// traffic-related memory to 1 GB under a packet flood, and historydb
	// is bounded against floods of random callsigns; the others have the
	// default.
	t.Run("memory", func(t *testing.T) {
		limits := map[string]float64{
			"pbuf_small_blocks_max":   192,
			"pbuf_medium_blocks_max":  128,
			"pbuf_large_blocks_max":   64,
			"dupecheck_blocks_max":    128,
			"historydb_blocks_max":    64,
			"client_heard_blocks_max": 200,
		}
		for k, want := range limits {
			if got, _ := j.Memory[k].(float64); got != want {
				t.Errorf("memory %s is %v, want %v", k, j.Memory[k], want)
			}
		}

		var total float64
		for _, a := range []string{"pbuf_small", "pbuf_medium", "pbuf_large", "dupecheck"} {
			blocks, _ := j.Memory[a+"_blocks_max"].(float64)
			size, _ := j.Memory[a+"_block_size"].(float64)
			total += blocks * size
		}
		if total != 1<<30 {
			t.Errorf("the traffic arenas may grow to %v bytes, want 1 GiB", total)
		}
	})

	s.Stop()
}

// httpConn is a keep-alive connection to the HTTP status port, driven by
// hand so that the test controls how many connections are open.
type httpConn struct {
	c net.Conn
	r *bufio.Reader
}

// dialHTTP opens a connection to the HTTP status port. It is closed when
// the test finishes.
func dialHTTP(t *testing.T, addr string) *httpConn {
	t.Helper()

	c, err := net.DialTimeout("tcp", addr, 5*time.Second)
	if err != nil {
		t.Fatalf("failed to connect to %s: %v", addr, err)
	}
	t.Cleanup(func() { c.Close() })
	return &httpConn{c: c, r: bufio.NewReader(c)}
}

// httpCounters is the "http" object of status.json.
type httpCounters struct {
	Clients             int `json:"clients"`
	ClientsMax          int `json:"clients_max"`
	Connections         int `json:"connections"`
	ConnectionsRejected int `json:"connections_rejected"`
}

// status fetches status.json on the connection, keeping it open, and
// returns its HTTP counters.
func (hc *httpConn) status(t *testing.T) httpCounters {
	t.Helper()

	req, err := http.NewRequest("GET", "http://aprsc/status.json", nil)
	if err != nil {
		t.Fatal(err)
	}
	hc.c.SetDeadline(time.Now().Add(5 * time.Second))
	if err := req.Write(hc.c); err != nil {
		t.Fatalf("failed to send a request: %v", err)
	}
	res, err := http.ReadResponse(hc.r, req)
	if err != nil {
		t.Fatalf("failed to read the response: %v", err)
	}
	defer res.Body.Close()
	if res.StatusCode != 200 {
		t.Fatalf("GET /status.json returned %s", res.Status)
	}

	var j struct {
		HTTP httpCounters `json:"http"`
	}
	if err := json.NewDecoder(res.Body).Decode(&j); err != nil {
		t.Fatalf("JSON decoding of status.json failed: %v", err)
	}
	return j.HTTP
}

// waitHTTP polls status.json on the connection until its HTTP counters
// satisfy ok, and returns them. The status document is cached for a second
// or two, so the counters lag behind the server a little.
func (hc *httpConn) waitHTTP(t *testing.T, what string, ok func(httpCounters) bool) httpCounters {
	t.Helper()

	deadline := time.Now().Add(8 * time.Second)
	for {
		st := hc.status(t)
		if ok(st) {
			return st
		}
		if time.Now().After(deadline) {
			t.Fatalf("status.json did not show %s in time, last: %+v", what, st)
		}
		time.Sleep(200 * time.Millisecond)
	}
}

// The number of open HTTP connections is limited: one over the limit is
// accepted and closed right away, and when one closes, there is room for
// a new one. The limit is set low here; by default it is what is left of
// the file descriptors above MaxClients.
func TestHTTPClientLimit(t *testing.T) {
	harness.Parallel(t)
	s := harness.StartText(t, "http-limit", configWith(t, "basic", "HTTPMaxClients 3"))

	addr := harness.AddrOn(t, "127.0.0.1", 55501)

	// three keep-alive connections, up to the limit, each in use
	var open []*httpConn
	for i := 1; i <= 3; i++ {
		open = append(open, dialHTTP(t, addr))
	}
	open[0].waitHTTP(t, "3 clients of max 3", func(st httpCounters) bool {
		return st.Clients == 3 && st.ClientsMax == 3
	})

	// the fourth one is closed by the server without a request being made
	over := dialHTTP(t, addr)
	over.c.SetReadDeadline(time.Now().Add(5 * time.Second))
	if _, err := over.r.ReadByte(); err == nil || isclient.IsTimeout(err) {
		t.Fatalf("the connection over the limit was not closed: %v", err)
	}
	open[0].waitHTTP(t, "the rejection", func(st httpCounters) bool {
		return st.Clients == 3 && st.Connections == 4 && st.ConnectionsRejected == 1
	})

	// closing one makes room for a new one
	open[2].c.Close()
	open[0].waitHTTP(t, "2 clients after a close", func(st httpCounters) bool {
		return st.Clients == 2
	})
	fresh := dialHTTP(t, addr)
	fresh.waitHTTP(t, "3 clients again", func(st httpCounters) bool {
		return st.Clients == 3 && st.ConnectionsRejected == 1
	})

	s.Stop()
}
