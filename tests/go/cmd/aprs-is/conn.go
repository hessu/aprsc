package main

import (
	"bufio"
	"bytes"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"regexp"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	fap "github.com/hessu/go-aprs-fap"
)

const appVersion = "2.0"

// client is a connection to an APRS-IS server. The tools do not wait for the
// login reply: like the Perl scripts they replace, they send the login line
// and go on, and the server's comment lines come in with the rest.
type client struct {
	conn net.Conn
	r    *bufio.Reader
	// the lines received up to the login reply, when dialTestServer read it
	preamble []string
}

// cwopRe matches the CWOP weather station callsigns, which log in read-only.
var cwopRe = regexp.MustCompile(`CW\d{4}`)

// passcode returns the passcode for a callsign, -1 for CWOP stations.
func passcode(call string) int {
	if cwopRe.MatchString(call) {
		return -1
	}
	return int(fap.AprsPasscode(call))
}

// loginLine builds the login line. udpPort, if not 0, asks the server to send
// the packets over UDP to that port.
func loginLine(call, app string, udpPort int, filter string) string {
	l := fmt.Sprintf("user %s pass %d vers aprs-is-%s %s", call, passcode(call), app, appVersion)
	if udpPort != 0 {
		l += fmt.Sprintf(" udp %d", udpPort)
	}
	if filter != "" {
		l += " filter " + filter
	}
	return l
}

// dial connects to server and logs in.
func dial(server, call, app string, udpPort int, filter string) (*client, error) {
	conn, err := net.DialTimeout("tcp", server, 10*time.Second)
	if err != nil {
		return nil, err
	}
	return login(conn, call, app, udpPort, filter)
}

// login sends the login line on a connection.
func login(conn net.Conn, call, app string, udpPort int, filter string) (*client, error) {
	if _, err := fmt.Fprintf(conn, "%s\r\n", loginLine(call, app, udpPort, filter)); err != nil {
		conn.Close()
		return nil, err
	}
	return &client{conn: conn, r: bufio.NewReaderSize(conn, 64*1024)}, nil
}

// logrespRe matches the login reply of a server, and its server id.
var logrespRe = regexp.MustCompile(`^# logresp \S+ \S+, server (\S+)`)

// loginReplyTimeout is how long dialTestServer waits for the login reply.
const loginReplyTimeout = 10 * time.Second

// dialTestServer is dial for the commands which send packets: it only
// connects to a test server, one on a loopback address whose server id starts
// with TEST, so that test traffic can not end up in the APRS-IS. The address
// is checked before anything is sent, and the server id is read from the
// login reply. The lines received up to and including the reply are returned
// in the client's preamble.
func dialTestServer(server, call, app string, udpPort int, filter string) (*client, error) {
	conn, err := net.DialTimeout("tcp", server, 10*time.Second)
	if err != nil {
		return nil, err
	}
	if err := checkLoopback(conn.RemoteAddr()); err != nil {
		conn.Close()
		return nil, err
	}
	c, err := login(conn, call, app, udpPort, filter)
	if err != nil {
		return nil, err
	}

	conn.SetReadDeadline(time.Now().Add(loginReplyTimeout))
	for {
		l, err := c.r.ReadString('\n')
		if err != nil {
			conn.Close()
			return nil, fmt.Errorf("%s: no login reply (%v), received: %q", server, err, c.preamble)
		}
		l = strings.TrimRight(l, "\r\n")
		c.preamble = append(c.preamble, l)
		if m := logrespRe.FindStringSubmatch(l); m != nil {
			if !strings.HasPrefix(m[1], "TEST") {
				conn.Close()
				return nil, fmt.Errorf("%s is not a test server: server id %q does not start with TEST", server, m[1])
			}
			break
		}
	}
	conn.SetReadDeadline(time.Time{})

	return c, nil
}

// checkLoopback returns an error if addr is not a loopback address.
func checkLoopback(addr net.Addr) error {
	tcp, ok := addr.(*net.TCPAddr)
	if !ok || !tcp.IP.IsLoopback() {
		return fmt.Errorf("%s is not a loopback address: packets are only sent to a test server on the local host", addr)
	}
	return nil
}

// dialUDP opens a UDP socket towards server for receiving the packets of a
// client logged in with "udp <port>", and returns it with its local port. Two
// comment lines are sent on it to open the way through a local firewall.
func dialUDP(server string) (*net.UDPConn, int, error) {
	addr, err := net.ResolveUDPAddr("udp", server)
	if err != nil {
		return nil, 0, err
	}
	u, err := net.DialUDP("udp", nil, addr)
	if err != nil {
		return nil, 0, err
	}
	for range 2 {
		u.Write([]byte("# pim\r\n"))
	}
	return u, u.LocalAddr().(*net.UDPAddr).Port, nil
}

// stats counts the lines and bytes received, and the open connections.
type stats struct {
	conns atomic.Int64
	lines atomic.Int64
	bytes atomic.Int64
}

// add counts a chunk of received data.
func (s *stats) add(b []byte) {
	s.bytes.Add(int64(len(b)))
	s.lines.Add(int64(bytes.Count(b, []byte{'\n'})))
}

// report prints the counters and the rates since the previous report to
// standard error every interval, until stop is closed.
func (s *stats) report(interval time.Duration, stop <-chan struct{}) {
	if interval <= 0 {
		return
	}
	t := time.NewTicker(interval)
	defer t.Stop()
	var lastLines, lastBytes int64
	last := time.Now()
	for {
		select {
		case <-stop:
			return
		case now := <-t.C:
			lines, b := s.lines.Load(), s.bytes.Load()
			secs := now.Sub(last).Seconds()
			fmt.Fprintf(os.Stderr, "%s connections %d, received %d lines (%.0f/s), %d bytes (%.0f kB/s)\n",
				now.Format("15:04:05"), s.conns.Load(), lines, float64(lines-lastLines)/secs,
				b, float64(b-lastBytes)/secs/1000)
			lastLines, lastBytes, last = lines, b, now
		}
	}
}

// statsWriter is an io.Writer which only counts what is written to it.
type statsWriter struct{ s *stats }

func (w statsWriter) Write(b []byte) (int, error) {
	w.s.add(b)
	return len(b), nil
}

// drain reads from r until EOF or an error, counting what was read.
func drain(r io.Reader, s *stats) error {
	buf := make([]byte, 64*1024)
	_, err := io.CopyBuffer(statsWriter{s}, r, buf)
	return err
}

// output is standard output, buffered, and shared by goroutines. Writers
// flush when they have nothing more to write for now.
type output struct {
	mu sync.Mutex
	w  *bufio.Writer
}

func newOutput() *output {
	return &output{w: bufio.NewWriterSize(os.Stdout, 64*1024)}
}

// line writes a line, prefixed by the Unix time if stamp is set, and flushes
// if flush is set.
func (o *output) line(l []byte, stamp, flush bool) {
	o.mu.Lock()
	defer o.mu.Unlock()
	if stamp {
		fmt.Fprintf(o.w, "%d\t", time.Now().Unix())
	}
	o.w.Write(l)
	o.w.WriteByte('\n')
	if flush {
		o.w.Flush()
	}
}

func (o *output) flush() {
	o.mu.Lock()
	o.w.Flush()
	o.mu.Unlock()
}

// trimEOL removes a trailing LF or CRLF.
func trimEOL(l []byte) []byte {
	l = bytes.TrimSuffix(l, []byte{'\n'})
	return bytes.TrimSuffix(l, []byte{'\r'})
}

// newFlags returns a flag set for a subcommand, with a usage message naming
// its arguments.
func newFlags(name, args, help string) *flag.FlagSet {
	fs := flag.NewFlagSet(name, flag.ExitOnError)
	fs.Usage = func() {
		fmt.Fprintf(fs.Output(), "Usage: aprs-is %s [options] %s\n\n%s\n\nOptions:\n", name, args, help)
		fs.PrintDefaults()
	}
	return fs
}

// parseArgs parses the options of a subcommand and checks the number of the
// arguments after them.
func parseArgs(fs *flag.FlagSet, args []string, min, max int) []string {
	fs.Parse(args)
	if fs.NArg() < min || fs.NArg() > max {
		fs.Usage()
		os.Exit(2)
	}
	return fs.Args()
}
