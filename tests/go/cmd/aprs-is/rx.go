package main

import (
	"bufio"
	"fmt"
	"os"
	"time"
)

func runRx(args []string) error {
	fs := newFlags("rx", "<mycall> <server:port>",
		"Logs in to the server and prints the lines it sends, comments included.\n"+
			"Packets received over UDP are always prefixed with the Unix time.")
	filter := fs.String("f", "", "`filter` to log in with")
	stamp := fs.Bool("t", false, "prefix each line with the Unix time it was received at")
	udp := fs.Bool("udp", false, "ask the server to send the packets over UDP")
	dur := fs.Duration("d", 0, "stop after this long (0: run until the server disconnects)")
	a := parseArgs(fs, args, 2, 2)
	call, server := a[0], a[1]

	out := newOutput()
	defer out.flush()

	udpPort := 0
	if *udp {
		u, port, err := dialUDP(server)
		if err != nil {
			return fmt.Errorf("UDP socket: %w", err)
		}
		defer u.Close()
		udpPort = port
		go func() {
			buf := make([]byte, 65536)
			for {
				n, err := u.Read(buf)
				if err != nil {
					return
				}
				out.line(trimEOL(buf[:n]), true, true)
			}
		}()
	}

	c, err := dial(server, call, "rx", udpPort, *filter)
	if err != nil {
		return err
	}
	defer c.conn.Close()
	fmt.Fprintln(os.Stderr, loginLine(call, "rx", udpPort, *filter))

	if *dur > 0 {
		time.AfterFunc(*dur, func() { c.conn.Close() })
	}

	return printLines(c, out, *stamp)
}

// printLines prints the lines received on c until the connection closes.
// Output is flushed whenever everything received so far has been printed.
func printLines(c *client, out *output, stamp bool) error {
	for {
		l, err := c.r.ReadSlice('\n')
		switch err {
		case nil:
			out.line(trimEOL(l), stamp, c.r.Buffered() == 0)
		case bufio.ErrBufferFull:
			// a line longer than the buffer is printed in pieces
			out.line(l, stamp, false)
		default:
			return nil
		}
	}
}
