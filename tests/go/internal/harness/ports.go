package harness

import (
	"fmt"
	"net"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"testing"
)

// The configurations in tests/cfg-aprsc use fixed port numbers, and the tests
// refer to the same numbers. To let tests run side by side, each running test
// gets a block of ports of its own, which maps each of those numbers to a port
// in a range of the block's own. The harness rewrites the configuration a
// server starts with accordingly, and the tests ask for their addresses with
// Port, Addr and AddrOn instead of spelling the numbers out.
//
// The blocks lie below the ephemeral port ranges of Linux (32768 and up) and
// macOS (49152 and up), unlike the numbers in the configurations: a port in
// the ephemeral range may be taken by any outgoing connection, the tests' own
// included, just when a server is about to bind it. The blocks are handed out
// in turn, so that a test does not bind the ports of a server which closed
// seconds ago.
//
// With another product than aprsc the numbers are used as they are, since its
// configurations are not rewritten, and the tests run one at a time.

// basePorts lists every port number which appears in the test configurations
// or is bound by a test, in ascending order. A port missing from here is a
// test error: Port fails on it, and so does starting a server with a
// configuration which has it.
var basePorts = []int{
	10153,               // fake upstream of basic, is2-basic and qconstr-protocols
	16404, 16405, 16406, // IPv4 UDP peer group: the server and two peers
	16504, 16505, 16506, // the IPv6 peer group
	51342,               // the UDP client's own port
	54153,               // fake upstreams of uplinks, IPv4 and IPv6
	55080,               // UDP submit and HTTP upload
	55152, 55153, 55154, // full feed, dupe feed, full feed TLS
	55501, 55502, // HTTP status of the first and the second server
	55580, 55581, 55582, 55583, // igate, client-only, and their TLS variants
	56152, 56153, 56580, 56581, // the IS2 ports
	57153, 57154, // fake upstreams of aggregator
}

const (
	// portBlockBase is where the port ranges of the blocks start: block n
	// gets portBlockBase+n*portBlockSize and the following ports, one per
	// entry of basePorts. maxBlocks keeps the ranges below the ephemeral
	// ports.
	portBlockBase = 20000
	portBlockSize = 100
	maxBlocks     = 64
)

// portBlock is the set of ports a test runs its servers and fake peers on.
type portBlock struct {
	index int
	// identity makes the block use the port numbers as they are, for the
	// products whose configurations are not rewritten.
	identity bool
	// started is set once a server has been started on the block
	started bool
}

// port maps a port number of the configurations to the block's.
func (b *portBlock) port(base int) (int, bool) {
	i := sort.SearchInts(basePorts, base)
	if i >= len(basePorts) || basePorts[i] != base {
		return 0, false
	}
	if b.identity {
		return base, true
	}
	return portBlockBase + b.index*portBlockSize + i, true
}

// statusAddr is the address of the HTTP status port of a test's first
// server.
func (b *portBlock) statusAddr() string {
	p, _ := b.port(55501)
	return net.JoinHostPort("127.0.0.1", strconv.Itoa(p))
}

var (
	blocksMu     sync.Mutex
	blocksInUse  = map[int]bool{}
	blocksByTest = map[string]*portBlock{}
	nextBlock    int // where the search for a free block starts
)

// rootName returns the name of the top-level test a subtest belongs to.
func rootName(name string) string {
	if i := strings.IndexByte(name, '/'); i >= 0 {
		return name[:i]
	}
	return name
}

// blockFor returns the port block of the test, allocating the next free one on
// the first call. The block is shared by the subtests of a test and released
// when the test, or subtest, which first used it finishes: a test which uses
// the harness after its subtests should do so before them too. Subtests must
// not run in parallel.
func blockFor(t *testing.T) *portBlock {
	t.Helper()

	root := rootName(t.Name())

	blocksMu.Lock()
	defer blocksMu.Unlock()

	if b := blocksByTest[root]; b != nil {
		return b
	}
	if !IsAprsc() && len(blocksInUse) > 0 {
		t.Fatalf("%s runs on the ports of its configurations only, so its tests cannot run in parallel", ProductName())
	}

	index := -1
	for n := 0; n < maxBlocks; n++ {
		if i := (nextBlock + n) % maxBlocks; !blocksInUse[i] {
			index = i
			break
		}
	}
	if index < 0 {
		t.Fatalf("all %d port blocks are in use; run fewer tests in parallel", maxBlocks)
	}
	nextBlock = (index + 1) % maxBlocks

	b := &portBlock{index: index, identity: !IsAprsc()}
	blocksInUse[index] = true
	blocksByTest[root] = b

	t.Cleanup(func() {
		blocksMu.Lock()
		defer blocksMu.Unlock()
		delete(blocksInUse, index)
		delete(blocksByTest, root)
	})

	return b
}

// Parallel marks the test as one which runs alongside other tests, on ports
// of its own; go test's -parallel flag limits how many run at a time. Call it
// first in a test. With another product than aprsc the tests run one at a
// time, on the ports of its configurations, which the harness does not
// rewrite.
func Parallel(t *testing.T) {
	t.Helper()
	if IsAprsc() {
		t.Parallel()
	}
}

// Port returns the port the test uses in place of base, one of the port
// numbers in the test configurations.
func Port(t *testing.T, base int) int {
	t.Helper()

	p, ok := blockFor(t).port(base)
	if !ok {
		t.Fatalf("port %d is not in the harness port table", base)
	}
	return p
}

// Addr returns the address "localhost:port" of Port(t, base).
func Addr(t *testing.T, base int) string {
	t.Helper()
	return AddrOn(t, "localhost", base)
}

// AddrOn returns the address "host:port" of Port(t, base) on a host.
func AddrOn(t *testing.T, host string, base int) string {
	t.Helper()
	return net.JoinHostPort(host, strconv.Itoa(Port(t, base)))
}

var (
	portTokenRe = regexp.MustCompile(`\b\d{4,5}\b`)
	portLineRe  = regexp.MustCompile(`(?i)^[ \t]*(Listen|Uplink|PeerGroup|HTTPStatus|HTTPUpload)\b`)
	runDirRe    = regexp.MustCompile(`(?im)^([ \t]*RunDir[ \t]+)\S+`)
)

// rewriteConfig returns an aprsc configuration with its ports mapped to the
// block's and its RunDir pointing at runDir. Every port number on a line
// which configures a listener, uplink, peer group or HTTP server must be in
// the port table, so that no server binds a port outside its block.
func (b *portBlock) rewriteConfig(text, runDir string) (string, error) {
	lines := strings.Split(text, "\n")

	continued := false // the previous line ended with a backslash
	for n, l := range lines {
		portLine := continued || portLineRe.MatchString(l)
		continued = strings.HasSuffix(strings.TrimRight(l, " \t"), `\`)

		var err error
		lines[n] = portTokenRe.ReplaceAllStringFunc(l, func(tok string) string {
			base, _ := strconv.Atoi(tok)
			p, ok := b.port(base)
			if !ok {
				if portLine && err == nil {
					err = fmt.Errorf("line %d: port %d is not in the harness port table", n+1, base)
				}
				return tok
			}
			return strconv.Itoa(p)
		})
		if err != nil {
			return "", err
		}
	}

	out := strings.Join(lines, "\n")
	out = runDirRe.ReplaceAllStringFunc(out, func(m string) string {
		return runDirRe.FindStringSubmatch(m)[1] + runDir
	})

	return out, nil
}
