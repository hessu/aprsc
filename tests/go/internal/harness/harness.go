// Package harness starts and stops the server under test for functional tests.
//
// The product is aprsc by default. Setting TEST_PRODUCT to javap or javap4
// runs the same tests against javAPRSSrvr, if a copy is present in
// tests/javaprssrvr or tests/javaprssrvr4 (they are not distributed).
//
// The server runs with the tests/ directory as its working directory (or
// tests/javaprssrvr4 for javap4), because the configurations refer to files
// relative to it (ACLs, TLS certificates, RunDir). The configurations also use
// fixed ports, so tests which start a server must not run in parallel.
package harness

import (
	"bytes"
	"errors"
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

// product describes how to run one of the supported servers.
type product struct {
	name string
	// dir is the working directory, relative to tests/.
	dir string
	// binary is the executable. It is resolved relative to dir if it is a
	// relative path with a slash in it.
	binary string
	// args are the fixed arguments before the configuration file.
	args []string
	// cfgFlag is placed before the configuration file path, if non-empty.
	cfgFlag string
	// cfgDir is the configuration directory, relative to dir.
	cfgDir string
	// exitCode is an exit status, besides 0, that means a clean shutdown
	// after SIGTERM. The JVM exits with 128+15.
	exitCode int
	// ownLogging is true for products which take -e, -o, -r and -p (aprsc).
	ownLogging bool
	// logFile is the log to show on failure, relative to tests/, for the
	// products without ownLogging.
	logFile string
}

var products = map[string]*product{
	"aprsc": {
		name:       "aprsc",
		dir:        ".",
		binary:     "", // see Binary()
		cfgFlag:    "-c",
		cfgDir:     "cfg-aprsc",
		ownLogging: true,
	},
	"javap": {
		name:     "javap",
		dir:      ".",
		binary:   "./javaprssrvr/java",
		args:     []string{"-server", "-cp", "./javaprssrvr/javAPRSSrvr.jar", "javAPRSSrvr"},
		cfgDir:   "cfg-javap",
		exitCode: 143,
		logFile:  "logs/err.log",
	},
	"javap4": {
		name:     "javap4",
		dir:      "javaprssrvr4",
		binary:   "../javaprssrvr/java",
		args:     []string{"-server", "-jar", "javAPRSSrvr.jar"},
		cfgDir:   ".",
		exitCode: 143,
		logFile:  "javaprssrvr4/err.log.0",
	},
}

// ProductName returns the name of the product under test: the value of
// TEST_PRODUCT, or "aprsc".
func ProductName() string {
	if p := os.Getenv("TEST_PRODUCT"); p != "" {
		return p
	}
	return "aprsc"
}

// IsAprsc reports whether the product under test is aprsc.
func IsAprsc() bool {
	return ProductName() == "aprsc"
}

// RequireAprsc skips the test unless the product under test is aprsc. Use it
// in tests of aprsc-specific features, such as IS2.
func RequireAprsc(t *testing.T) {
	t.Helper()
	if !IsAprsc() {
		t.Skipf("test requires aprsc, TEST_PRODUCT is %q", ProductName())
	}
}

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

// Server is a running server process.
type Server struct {
	t       *testing.T
	prod    *product
	cmd     *exec.Cmd
	logDir  string
	output  bytes.Buffer
	done    chan struct{} // closed when the process has been reaped
	waitErr error         // result of cmd.Wait, valid after done is closed
	stopped bool
}

// Start runs the product with the named configuration (from tests/cfg-aprsc,
// tests/cfg-javap or the javap4 directory) and waits until the HTTP status
// port accepts connections. The server is stopped when the test finishes, and
// its log is printed if the test failed.
func Start(t *testing.T, cfg string, extraArgs ...string) *Server {
	t.Helper()
	return start(t, cfg, true, extraArgs)
}

// StartAdditional starts a second server in a test, with a configuration which
// uses ports different from the first one. It does not insist on the status
// port being free.
func StartAdditional(t *testing.T, cfg string, extraArgs ...string) *Server {
	t.Helper()
	return start(t, cfg, false, extraArgs)
}

// CheckConfig has aprsc parse a configuration and exit. It returns aprsc's
// output, and an error if the configuration was not accepted. A test can use
// it to see whether the aprsc build has an optional feature, such as SCTP.
func CheckConfig(t *testing.T, cfg string) (string, error) {
	t.Helper()

	if !IsAprsc() {
		t.Fatalf("CheckConfig only works with aprsc")
	}

	cmd := exec.Command(Binary(), "-y", "-c", filepath.Join("cfg-aprsc", cfg), "-e", "info", "-o", "stderr")
	cmd.Dir = TestsDir()
	out, err := cmd.CombinedOutput()
	return string(out), err
}

func start(t *testing.T, cfg string, checkStale bool, extraArgs []string) *Server {
	t.Helper()

	prod, ok := products[ProductName()]
	if !ok {
		t.Fatalf("unknown TEST_PRODUCT %q", ProductName())
	}

	dir := filepath.Join(TestsDir(), prod.dir)

	bin := prod.binary
	if prod.name == "aprsc" {
		bin = Binary()
	}
	binPath := bin
	if !filepath.IsAbs(binPath) {
		binPath = filepath.Join(dir, binPath)
	}
	if _, err := os.Stat(binPath); err != nil {
		t.Fatalf("%s executable not found (aprsc must be built in src/ first): %v", prod.name, err)
	}

	cfgFile := filepath.Join(prod.cfgDir, cfg)
	if _, err := os.Stat(filepath.Join(dir, cfgFile)); err != nil {
		if !prod.ownLogging {
			// not every test has a configuration for the other products
			t.Skipf("no %s configuration for this test: %v", prod.name, err)
		}
		t.Fatalf("no such configuration file: %v", err)
	}

	// A leftover server would hold the ports and answer in place of ours.
	if checkStale {
		if c, err := net.DialTimeout("tcp", StatusAddr, time.Second); err == nil {
			c.Close()
			t.Fatalf("something is already listening on %s; is an old server still running?", StatusAddr)
		}
	}

	s := &Server{
		t:    t,
		prod: prod,
		done: make(chan struct{}),
	}

	var args []string
	if prod.ownLogging {
		s.logDir = t.TempDir()
		args = append(args,
			"-e", "debug", "-o", "file", "-r", s.logDir,
			"-p", filepath.Join(s.logDir, "aprsc.pid"))
	}
	args = append(args, prod.args...)
	if prod.cfgFlag != "" {
		args = append(args, prod.cfgFlag)
	}
	args = append(args, cfgFile)
	args = append(args, extraArgs...)

	s.cmd = exec.Command(bin, args...)
	s.cmd.Dir = dir
	s.cmd.Env = os.Environ()
	if prod.ownLogging {
		s.cmd.Env = append(s.cmd.Env, "APRSC_NO_VERSION_REPORT=1")
	}
	s.cmd.Stdout = &s.output
	s.cmd.Stderr = &s.output

	if err := s.cmd.Start(); err != nil {
		t.Fatalf("failed to run %s: %v", prod.name, err)
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
			return fmt.Errorf("%s quit during startup: %v", s.prod.name, s.waitErr)
		default:
		}

		if s.prod.ownLogging {
			// Some configurations have no status port, so watch the log
			// for the accept thread having opened the listeners instead.
			if b, err := os.ReadFile(s.LogFile()); err == nil && bytes.Contains(b, []byte("Accept thread ready")) {
				return nil
			}
		} else if c, err := net.DialTimeout("tcp", StatusAddr, 200*time.Millisecond); err == nil {
			c.Close()
			return nil
		}
		time.Sleep(50 * time.Millisecond)
	}

	return fmt.Errorf("%s was not ready within %v", s.prod.name, startTimeout)
}

// cleanExit reports whether err, the result of waiting for the process after
// SIGTERM, means that the product shut down normally.
func (p *product) cleanExit(err error) bool {
	if err == nil {
		return true
	}

	var ee *exec.ExitError
	if !errors.As(err, &ee) {
		return false
	}
	if p.exitCode != 0 && ee.ExitCode() == p.exitCode {
		return true
	}
	if ws, ok := ee.Sys().(syscall.WaitStatus); ok && p.exitCode != 0 {
		// the JVM may also die of the signal itself
		return ws.Signaled() && ws.Signal() == syscall.SIGTERM
	}

	return false
}

// Stop terminates the server with SIGTERM and fails the test if it does not
// exit cleanly. A server that ignores SIGTERM is killed and reported as a
// failure.
func (s *Server) Stop() {
	s.t.Helper()

	if s.stopped {
		return
	}
	s.stopped = true

	select {
	case <-s.done:
		s.t.Errorf("%s had already exited: %v", s.prod.name, s.waitErr)
		return
	default:
	}

	s.cmd.Process.Signal(syscall.SIGTERM)

	select {
	case <-s.done:
	case <-time.After(stopTimeout):
		s.cmd.Process.Kill()
		<-s.done
		s.t.Errorf("%s did not exit within %v of SIGTERM, killed it", s.prod.name, stopTimeout)
		return
	}

	if !s.prod.cleanExit(s.waitErr) {
		s.t.Errorf("%s exited uncleanly: %v", s.prod.name, s.waitErr)
	}
}

// Signal sends a signal to the server, for example SIGUSR1 to reload the
// configuration. It fails the test if the server is not running.
func (s *Server) Signal(sig syscall.Signal) {
	s.t.Helper()

	select {
	case <-s.done:
		s.t.Fatalf("cannot signal %s, it has exited: %v", s.prod.name, s.waitErr)
	default:
	}

	if err := s.cmd.Process.Signal(sig); err != nil {
		s.t.Fatalf("failed to signal %s: %v", s.prod.name, err)
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

// LogFile returns the path of the server's log file. For the Java products
// this is a log shared between runs.
func (s *Server) LogFile() string {
	if s.prod.ownLogging {
		return filepath.Join(s.logDir, "aprsc.log")
	}
	return filepath.Join(TestsDir(), s.prod.logFile)
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
		s.t.Logf("%s (last 8 kB):\n%s", filepath.Base(s.LogFile()), tail(b, 8192))
	}
	if s.output.Len() > 0 {
		s.t.Logf("%s stdout/stderr:\n%s", s.prod.name, tail(s.output.Bytes(), 4096))
	}
}

func tail(b []byte, n int) []byte {
	if len(b) > n {
		return b[len(b)-n:]
	}
	return b
}
