package main

import (
	"fmt"
	"os"
	"strconv"
	"sync/atomic"
	"time"
)

func runMultiRx(args []string) error {
	fs := newFlags("multirx", "<callsign> <server:port> <count>",
		"Opens count receiving connections to the server, logged in as\n"+
			"<callsign>-0, <callsign>-1 and so on, and discards what they receive. A\n"+
			"connection which the server closes is opened again.")
	filter := fs.String("f", "p/OH/OI/OJ/OF", "`filter` to log in with")
	statsEvery := fs.Duration("stats", 10*time.Second, "interval of the statistics printed (0: none)")
	a := parseArgs(fs, args, 3, 3)
	prefix, server := a[0], a[1]
	n, err := strconv.Atoi(a[2])
	if err != nil || n < 1 {
		return fmt.Errorf("bad connection count %q", a[2])
	}

	fmt.Fprintf(os.Stderr, "Connecting to %s as %s-* with %d sockets\n", server, prefix, n)

	var st stats
	go st.report(*statsEvery, nil)

	connect := func(call string) *client {
		for {
			c, err := dial(server, call, "multirx", 0, *filter)
			if err == nil {
				st.conns.Add(1)
				return c
			}
			fmt.Fprintf(os.Stderr, "%s: %v\n", call, err)
			time.Sleep(time.Second)
		}
	}

	// The connections are opened one at a time, not to overflow the
	// server's listen queue, and then read in goroutines of their own.
	var eofs atomic.Int64
	for i := range n {
		call := fmt.Sprintf("%s-%d", prefix, i)
		c := connect(call)
		go func() {
			for {
				drain(c.r, &st)
				c.conn.Close()
				st.conns.Add(-1)
				fmt.Fprintf(os.Stderr, "eof %d (%s), reconnecting\n", eofs.Add(1), call)
				c = connect(call)
			}
		}()
	}
	select {}
}
