package functional

import (
	"fmt"
	"strings"
	"testing"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// Parsing of greatly repeated filters: a single filter gets extended up to
// hundreds of entries, one connection after another. Port of
// t/38filter-verylong.t.
//
// TODO: run this threaded, with multiple copies, for a better chance of
// finding a bug.
func TestFilterVeryLong(t *testing.T) {
	harness.Parallel(t)
	s := harness.Start(t, "basic")

	const rounds = 300

	for i := 0; i < rounds; i++ {
		var filters []string
		for b := 0; b < i; b++ {
			filters = append(filters, fmt.Sprintf("b/BUDDYYYY%d-42", b))
		}

		c := isclient.Connect(t, harness.Addr(t, 55580), "N5CAL-1",
			isclient.WithFilter(strings.Join(filters, " ")), isclient.WithPasscode("-1"))
		c.Close()

		if t.Failed() {
			break
		}
	}

	s.Stop()
}
