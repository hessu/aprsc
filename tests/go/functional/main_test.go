package functional

import (
	"os"
	"testing"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

func TestMain(m *testing.M) {
	// only aprsc replies to "#filter" commands
	isclient.FilterAck = harness.IsAprsc()

	os.Exit(m.Run())
}
