package functional

import (
	"testing"
	"time"

	"github.com/hessu/aprsc/tests/go/internal/harness"
)

// Start the server and stop it cleanly. Port of t/00startstop.t.
func TestStartStop(t *testing.T) {
	s := harness.Start(t, "basic")

	time.Sleep(time.Second)
	if !s.Running() {
		t.Fatal("server exited by itself after startup")
	}

	s.Stop()
}
