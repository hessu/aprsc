package main

import (
	"bufio"
	"fmt"
	"os"
	"sync/atomic"
	"time"
)

func runCopy(args []string) error {
	fs := newFlags("copy", "<mycall> <from-server:port> <to-server:port>",
		"Logs in to both servers and sends the packets received from the first one to\n"+
			"the second one, for loading a test server with real traffic. Comment lines\n"+
			"are not copied, and what the second server sends is discarded.")
	filter := fs.String("f", "", "`filter` to log in to the first server with")
	dur := fs.Duration("d", 10*time.Minute, "stop after this long (0: run until a server disconnects)")
	statsEvery := fs.Duration("stats", 10*time.Second, "interval of the statistics printed (0: none)")
	a := parseArgs(fs, args, 3, 3)
	call, from, to := a[0], a[1], a[2]

	dst, err := dial(to, call, "copy", 0, "")
	if err != nil {
		return fmt.Errorf("%s: %w", to, err)
	}
	defer dst.conn.Close()
	src, err := dial(from, call, "copy", 0, *filter)
	if err != nil {
		return fmt.Errorf("%s: %w", from, err)
	}
	defer src.conn.Close()
	fmt.Fprintln(os.Stderr, loginLine(call, "copy", 0, *filter))

	var st stats
	st.conns.Store(2)
	stop := make(chan struct{})
	defer close(stop)
	go st.report(*statsEvery, stop)
	var closed atomic.Bool
	go func() {
		drain(dst.r, &stats{})
		if closed.CompareAndSwap(false, true) {
			fmt.Fprintf(os.Stderr, "%s closed the connection\n", to)
		}
		src.conn.Close()
	}()
	if *dur > 0 {
		time.AfterFunc(*dur, func() {
			closed.Store(true)
			src.conn.Close()
		})
	}

	w := bufio.NewWriterSize(dst.conn, 64*1024)
	copied := 0
	for {
		l, err := src.r.ReadSlice('\n')
		if err == bufio.ErrBufferFull {
			// too long to be a packet; skip the rest of it
			for err == bufio.ErrBufferFull {
				_, err = src.r.ReadSlice('\n')
			}
			continue
		}
		if err != nil {
			if closed.CompareAndSwap(false, true) {
				fmt.Fprintf(os.Stderr, "%s closed the connection\n", from)
			}
			w.Flush()
			fmt.Fprintf(os.Stderr, "copied %d packets\n", copied)
			return nil
		}
		st.add(l)
		if l[0] == '#' {
			continue
		}
		copied++
		if _, err := w.Write(l); err != nil {
			return fmt.Errorf("%s: %w", to, err)
		}
		if src.r.Buffered() == 0 {
			if err := w.Flush(); err != nil {
				return fmt.Errorf("%s: %w", to, err)
			}
		}
	}
}
