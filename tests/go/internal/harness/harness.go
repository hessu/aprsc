// Package harness starts and stops the aprsc binary for functional tests.
//
// The server runs with the tests/ directory as its working directory, because
// the configurations in tests/cfg-aprsc refer to files relative to it (ACLs,
// TLS certificates, RunDir). The configurations also use fixed ports, so
// tests which start a server must not run in parallel.
package harness

import (
	"bytes"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"syscall"
	"testing"
	"time"
)

const (
	// StatusAddr is the HTTP status port in the test configurations. The
	// harness uses it to tell when the server is up.
	StatusAddr = "127.0.0.1:55501"

	startTimeout = 5 * time.Second
	stopTimeout  = 6 * time.Second
)

// TestsDir returns the absolute path of the repository's tests/ directory.
// It can be overridden with APRSC_TESTS_DIR.
func TestsDir() string {
	if d := os.Getenv("APRSC_TESTS_DIR"); d != "" {
		return d
	}
	_, file, _, _ := runtime.Caller(0)
	// <tests>/go/internal/harness/harness.go
	return filepath.Join(filepath.Dir(file), "..", "..", "..")
}

// Binary returns the path of the aprsc binary under test. It can be
// overridden with APRSC_BIN.
func Binary() string {
	if b := os.Getenv("APRSC_BIN"); b != "" {
		return b
	}
	return filepath.Join(TestsDir(), "..", "src", "aprsc")
}

// Server is a running aprsc process.
type Server struct {
	t       *testing.T
	cmd     *exec.Cmd
	logDir  string
	output  bytes.Buffer
	done    chan struct{} // closed when the process has been reaped
	waitErr error         // result of cmd.Wait, valid after done is closed
	stopped bool
}

// Start runs aprsc with the named configuration from tests/cfg-aprsc and waits
// until the HTTP status port accepts connections. The server is stopped when
// the test finishes, and its log is printed if the test failed.
func Start(t *testing.T, cfg string, extraArgs ...string) *Server {
	t.Helper()

	bin := Binary()
	if _, err := os.Stat(bin); err != nil {
		t.Fatalf("aprsc binary not found (build it in src/ first): %v", err)
	}

	cfgFile := filepath.Join("cfg-aprsc", cfg)
	if _, err := os.Stat(filepath.Join(TestsDir(), cfgFile)); err != nil {
		t.Fatalf("no such configuration file: %v", err)
	}

	// A leftover server would hold the ports and answer in place of ours.
	if c, err := net.DialTimeout("tcp", StatusAddr, time.Second); err == nil {
		c.Close()
		t.Fatalf("something is already listening on %s; is an old aprsc still running?", StatusAddr)
	}

	s := &Server{
		t:      t,
		logDir: t.TempDir(),
		done:   make(chan struct{}),
	}

	args := []string{
		"-e", "debug", "-o", "file", "-r", s.logDir,
		"-p", filepath.Join(s.logDir, "aprsc.pid"),
		"-c", cfgFile,
	}
	args = append(args, extraArgs...)

	s.cmd = exec.Command(bin, args...)
	s.cmd.Dir = TestsDir()
	s.cmd.Env = append(os.Environ(), "APRSC_NO_VERSION_REPORT=1")
	s.cmd.Stdout = &s.output
	s.cmd.Stderr = &s.output

	if err := s.cmd.Start(); err != nil {
		t.Fatalf("failed to run aprsc: %v", err)
	}

	go func() {
		s.waitErr = s.cmd.Wait()
		close(s.done)
	}()

	t.Cleanup(s.cleanup)

	if err := s.waitReady(); err != nil {
		t.Fatalf("%v", err)
	}

	return s
}

func (s *Server) waitReady() error {
	deadline := time.Now().Add(startTimeout)
	for time.Now().Before(deadline) {
		select {
		case <-s.done:
			return fmt.Errorf("aprsc quit during startup: %v", s.waitErr)
		default:
		}

		if c, err := net.DialTimeout("tcp", StatusAddr, 200*time.Millisecond); err == nil {
			c.Close()
			return nil
		}
		time.Sleep(50 * time.Millisecond)
	}

	return fmt.Errorf("aprsc did not open %s within %v", StatusAddr, startTimeout)
}

// Stop terminates the server with SIGTERM and fails the test if it does not
// exit cleanly with status 0. A server that ignores SIGTERM is killed and
// reported as a failure.
func (s *Server) Stop() {
	s.t.Helper()

	if s.stopped {
		return
	}
	s.stopped = true

	select {
	case <-s.done:
		s.t.Errorf("aprsc had already exited: %v", s.waitErr)
		return
	default:
	}

	s.cmd.Process.Signal(syscall.SIGTERM)

	select {
	case <-s.done:
	case <-time.After(stopTimeout):
		s.cmd.Process.Kill()
		<-s.done
		s.t.Errorf("aprsc did not exit within %v of SIGTERM, killed it", stopTimeout)
		return
	}

	if s.waitErr != nil {
		s.t.Errorf("aprsc exited uncleanly: %v", s.waitErr)
	}
}

// Signal sends a signal to the server, for example SIGUSR1 to reload the
// configuration. It fails the test if the server is not running.
func (s *Server) Signal(sig syscall.Signal) {
	s.t.Helper()

	select {
	case <-s.done:
		s.t.Fatalf("cannot signal aprsc, it has exited: %v", s.waitErr)
	default:
	}

	if err := s.cmd.Process.Signal(sig); err != nil {
		s.t.Fatalf("failed to signal aprsc: %v", err)
	}
}

// Running reports whether the server process is still alive.
func (s *Server) Running() bool {
	select {
	case <-s.done:
		return false
	default:
		return true
	}
}

// LogFile returns the path of the server's log file.
func (s *Server) LogFile() string {
	return filepath.Join(s.logDir, "aprsc.log")
}

// cleanup runs at the end of the test: it makes sure the process is gone, and
// shows the server's output if the test failed.
func (s *Server) cleanup() {
	if !s.stopped {
		s.stopped = true
		select {
		case <-s.done:
		default:
			s.cmd.Process.Kill()
			<-s.done
		}
	}

	if !s.t.Failed() {
		return
	}

	if b, err := os.ReadFile(s.LogFile()); err == nil {
		s.t.Logf("aprsc.log (last 8 kB):\n%s", tail(b, 8192))
	}
	if s.output.Len() > 0 {
		s.t.Logf("aprsc stdout/stderr:\n%s", tail(s.output.Bytes(), 4096))
	}
}

func tail(b []byte, n int) []byte {
	if len(b) > n {
		return b[len(b)-n:]
	}
	return b
}
