package functional

import (
	"flag"
	"os"
	"strconv"
	"testing"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// defaultParallel is how many tests run at a time unless -parallel says
// otherwise. Each one runs a server of its own, so a few at a time is plenty;
// go test's own default would be GOMAXPROCS.
const defaultParallel = 3

func TestMain(m *testing.M) {
	flag.Parse()
	parallelSet := false
	flag.Visit(func(f *flag.Flag) {
		if f.Name == "test.parallel" {
			parallelSet = true
		}
	})
	if !parallelSet {
		flag.Set("test.parallel", strconv.Itoa(defaultParallel))
	}

	// only aprsc replies to "#filter" commands
	isclient.FilterAck = harness.IsAprsc()

	os.Exit(m.Run())
}
