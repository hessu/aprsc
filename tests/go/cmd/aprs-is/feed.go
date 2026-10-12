package main

import (
	"bufio"
	"fmt"
	"math/rand/v2"
	"os"
	"time"
)

func runFeed(args []string) error {
	fs := newFlags("feed", "<server:port> < packets",
		"Sends the packets read from standard input to the server, as fast as it\n"+
			"takes them. The input lines are prefixed with a Unix time and a space or\n"+
			"tab, as \"aprs-is rx -t\" writes them, and lines without one are skipped,\n"+
			"unless -raw is given. The lines the server sends are printed to standard\n"+
			"error.")
	call := fs.String("c", fmt.Sprintf("F0FED-%d", rand.IntN(15)), "`callsign` to log in with")
	raw := fs.Bool("raw", false, "the input lines are packets without a time prefix")
	batch := fs.Int("batch", 3, "send the packets in batches of this many, one write each")
	pause := fs.Duration("pause", 0, "pause for a random time up to this long now and then,\n"+
		"for letting the server's packet buffers expire (0: never)")
	pauseEvery := fs.Duration("pause-every", 3*time.Second, "random maximum time between the pauses")
	a := parseArgs(fs, args, 1, 1)
	server := a[0]

	c, err := dial(server, *call, "feed", 0, "")
	if err != nil {
		return err
	}
	defer c.conn.Close()
	fmt.Fprintln(os.Stderr, loginLine(*call, "feed", 0, ""))

	go func() {
		s := bufio.NewScanner(c.r)
		for s.Scan() {
			fmt.Fprintf(os.Stderr, "got: %s\n", s.Text())
		}
	}()

	randDur := func(max time.Duration) time.Duration {
		if max <= 0 {
			return 0
		}
		return rand.N(max)
	}
	nextPause := time.Now().Add(randDur(*pause))

	in := bufio.NewReaderSize(os.Stdin, 64*1024)
	w := bufio.NewWriterSize(c.conn, 64*1024)
	n, inBatch := 0, 0
	for {
		l, err := in.ReadSlice('\n')
		if err == bufio.ErrBufferFull {
			for err == bufio.ErrBufferFull {
				_, err = in.ReadSlice('\n')
			}
			continue
		}
		if len(l) == 0 && err != nil {
			break
		}
		l = trimEOL(l)
		if !*raw {
			if l = stripTime(l); l == nil {
				continue
			}
		}

		if *pause > 0 && time.Now().After(nextPause) {
			if err := w.Flush(); err != nil {
				return err
			}
			p := randDur(*pause)
			fmt.Fprintf(os.Stderr, "sleeping %.3f s\n", p.Seconds())
			time.Sleep(p)
			nextPause = time.Now().Add(randDur(*pauseEvery))
		}

		w.Write(l)
		w.WriteString("\r\n")
		n++
		inBatch++
		if inBatch >= *batch {
			if err := w.Flush(); err != nil {
				return err
			}
			inBatch = 0
		}
		if err != nil {
			break
		}
	}
	if err := w.Flush(); err != nil {
		return err
	}
	fmt.Fprintf(os.Stderr, "sent %d packets\n", n)
	return nil
}

// stripTime removes the "<unix time> " or "<unix time>\t" prefix of a line, and
// returns nil if it has none.
func stripTime(l []byte) []byte {
	i := 0
	for i < len(l) && l[i] >= '0' && l[i] <= '9' {
		i++
	}
	if i == 0 || i == len(l) || (l[i] != ' ' && l[i] != '\t') {
		return nil
	}
	return l[i+1:]
}
