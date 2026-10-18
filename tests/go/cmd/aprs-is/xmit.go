package main

import (
	"bufio"
	"fmt"
	"os"
	"time"
)

// samplePackets are sent by xmit when no file is given: one of each common
// kind of position packet, with comment lines describing them.
var samplePackets = []string{
	"# position w/o timestamp",
	"JH6YLM>APRS,RELAY,TRACE5-5,qAo,JA6JMJ-3:!3210.70N/13132.15E#15 KAWA",
	"PY3KN-1>WIDE1-1,TCPIP*,qAC,T2BRAZIL:=3003.96SI05106.10W&iGate Viamao",
	"PD0TK-9>APERXQ,PA3GKF-2*,WIDE2-1,qAo,DB0SDA:!5057.18N/00549.40E>037/004/A=000353",
	"# position timestamp and WEATHER telemetry",
	"N0YNC>APRS,TCPXX*,qAX,T2PSR:@271607z4028.82N/09657.64W_272/003g004t036r000P000p000h62b10206v31",
	"# OBJECT position with timestamp",
	"DB0XIP>APU25N,TCPIP*,qAC,THIRD:;DF0OV *181515z4915.09N/00725.45E-K35 www.k35-schwarzbachtal.de",
	"# compressed position",
	`OH7LZB-9>APZMDR,WIDE3-3,qAo,OH2RCH:!/0"acTjK">?S_ http://aprs.fi/`,
	"# mic-e position",
	`KG4PKR-1>SUSV9P,W4GGM-1,KA4BNI-2*,WIDE2,qAR,KB4YTM-1:'sZ&l!^K]"6)}`,
}

func runXmit(args []string) error {
	fs := newFlags("xmit", "<mycall> <server:port> [file]",
		"Sends packets to the server one at a time, and prints the lines it sends\n"+
			"back prefixed with the Unix time. The packets are read from the file,\n"+
			"one per line, or are a built-in set of samples. Exits one interval after\n"+
			"sending the last one. Only sends to a test server: one on a loopback\n"+
			"address, with a server id starting with TEST.")
	filter := fs.String("f", "", "`filter` to log in with")
	interval := fs.Duration("i", 5*time.Second, "interval between the packets sent")
	a := parseArgs(fs, args, 2, 3)
	call, server := a[0], a[1]

	pkts := samplePackets
	if len(a) == 3 {
		var err error
		if pkts, err = readLines(a[2]); err != nil {
			return err
		}
	}

	c, err := dialTestServer(server, call, "xmit", 0, *filter)
	if err != nil {
		return err
	}
	defer c.conn.Close()
	fmt.Fprintln(os.Stderr, loginLine(call, "xmit", 0, *filter))

	out := newOutput()
	defer out.flush()
	for _, l := range c.preamble {
		out.line([]byte(l), true, true)
	}
	done := make(chan error, 1)
	go func() { done <- printLines(c, out, true) }()

	t := time.NewTicker(*interval)
	defer t.Stop()
	for _, p := range pkts {
		select {
		case <-done:
			return fmt.Errorf("server closed the connection")
		case <-t.C:
		}
		if _, err := fmt.Fprintf(c.conn, "%s\r\n", p); err != nil {
			return err
		}
	}
	select {
	case <-done:
	case <-t.C:
	}
	return nil
}

func readLines(name string) ([]string, error) {
	f, err := os.Open(name)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	var lines []string
	s := bufio.NewScanner(f)
	for s.Scan() {
		lines = append(lines, s.Text())
	}
	return lines, s.Err()
}
