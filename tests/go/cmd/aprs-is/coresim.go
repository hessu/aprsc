package main

import (
	"bufio"
	_ "embed"
	"fmt"
	"net"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"
)

// coreSimClients is the client list of the core servers in March 2008.
//
//go:embed coresim.txt
var coreSimClients string

// coreSimPorts maps the ports of the 2008 core servers to the ports of a test
// server: 23 is not available to a server which is not run as root, and the
// history feeding ports are served by the plain ones.
var coreSimPorts = map[int]int{
	23:    10152,
	10151: 10152,
	10153: 10152,
	10253: 14580,
	20153: 20152,
}

type coreSimClient struct {
	port   int
	udp    bool
	call   string
	filter string
}

func parseCoreSim(text string, ports map[int]int) ([]coreSimClient, error) {
	var clients []coreSimClient
	s := bufio.NewScanner(strings.NewReader(text))
	for s.Scan() {
		l := strings.TrimSpace(s.Text())
		if l == "" || l[0] == '#' {
			continue
		}
		f := strings.SplitN(strings.Join(strings.Fields(l), " "), " ", 3)
		if len(f) < 2 {
			return nil, fmt.Errorf("bad client line %q", l)
		}
		c := coreSimClient{call: f[1]}
		p, udp := strings.CutSuffix(f[0], "u")
		port, err := strconv.Atoi(p)
		if err != nil {
			return nil, fmt.Errorf("bad port in client line %q", l)
		}
		if to, ok := ports[port]; ok {
			port = to
		}
		c.port, c.udp = port, udp
		if len(f) == 3 {
			c.filter = f[2]
		}
		clients = append(clients, c)
	}
	return clients, nil
}

// portMap is a flag of port mappings, "from=to", which can be given many
// times.
type portMap map[int]int

func (m portMap) String() string { return "" }

func (m portMap) Set(v string) error {
	from, to, ok := strings.Cut(v, "=")
	f, err1 := strconv.Atoi(from)
	t, err2 := strconv.Atoi(to)
	if !ok || err1 != nil || err2 != nil {
		return fmt.Errorf("expected <port>=<port>")
	}
	m[f] = t
	return nil
}

func runCoreSim(args []string) error {
	fs := newFlags("coresim", "",
		"Connects the clients of the three APRS-IS core servers on one day in\n"+
			"March 2008 to a test server, logged in with the callsigns and filters they\n"+
			"had, to see how much load serving them causes. None of them send anything,\n"+
			"and what they receive is discarded. Exits when the server has closed all\n"+
			"the connections.\n\n"+
			"The 2008 ports 23, 10151 and 10153 are mapped to 10152, 10253 to 14580\n"+
			"and 20153 to 20152, and the rest are used as they are: the clients of\n"+
			"ports the test server does not listen on fail to connect, unless their\n"+
			"port is mapped with -map.")
	host := fs.String("host", "localhost", "the server's `address`")
	ports := portMap{}
	for k, v := range coreSimPorts {
		ports[k] = v
	}
	fs.Var(ports, "map", "map a port to another, `from=to`, can be given many times")
	statsEvery := fs.Duration("stats", 10*time.Second, "interval of the statistics printed (0: none)")
	dur := fs.Duration("d", 0, "stop after this long (0: run until the server disconnects everyone)")
	parseArgs(fs, args, 0, 0)

	clients, err := parseCoreSim(coreSimClients, ports)
	if err != nil {
		return err
	}

	var st stats
	stop := make(chan struct{})
	go st.report(*statsEvery, stop)

	var wg sync.WaitGroup
	var mu sync.Mutex
	var conns []net.Conn
	failed := 0
	for _, cl := range clients {
		server := net.JoinHostPort(*host, strconv.Itoa(cl.port))
		udpPort := 0
		if cl.udp {
			u, port, err := dialUDP(server)
			if err != nil {
				fmt.Fprintf(os.Stderr, "%s: UDP socket: %v\n", cl.call, err)
				failed++
				continue
			}
			udpPort = port
			mu.Lock()
			conns = append(conns, u)
			mu.Unlock()
			go drain(u, &st)
		}
		c, err := dial(server, cl.call, "coresim", udpPort, cl.filter)
		if err != nil {
			fmt.Fprintf(os.Stderr, "%s: %v\n", cl.call, err)
			failed++
			continue
		}
		mu.Lock()
		conns = append(conns, c.conn)
		mu.Unlock()
		st.conns.Add(1)
		wg.Add(1)
		go func() {
			defer wg.Done()
			drain(c.r, &st)
			c.conn.Close()
			st.conns.Add(-1)
		}()
	}
	fmt.Fprintf(os.Stderr, "%d clients connected, %d failed\n", len(clients)-failed, failed)

	if *dur > 0 {
		time.AfterFunc(*dur, func() {
			mu.Lock()
			defer mu.Unlock()
			for _, c := range conns {
				c.Close()
			}
		})
	}
	wg.Wait()
	close(stop)
	mu.Lock()
	for _, c := range conns {
		c.Close()
	}
	mu.Unlock()
	fmt.Fprintf(os.Stderr, "received %d lines, %d bytes\n", st.lines.Load(), st.bytes.Load())
	return nil
}
