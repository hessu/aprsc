package main

import (
	"context"
	"fmt"
	"io"
	"net"
	"net/http"
	"strings"
	"time"
)

func runPost(args []string) error {
	fs := newFlags("post", "[packet ...]",
		"Sends packets to the server's HTTP position upload port in one POST, and\n"+
			"prints the reply. Without arguments, one test packet is sent. Only sends\n"+
			"to a test server: the URL must be on a loopback address, and the server\n"+
			"id read on the APRS-IS port given with -server must start with TEST.")
	url := fs.String("url", "http://127.0.0.1:55080/", "the `URL` to post to")
	server := fs.String("server", "127.0.0.1:55152", "an APRS-IS `host:port` of the same server, for checking its id")
	call := fs.String("c", "TEST", "`callsign` to log in with")
	a := parseArgs(fs, args, 0, 1<<20)
	if len(a) == 0 {
		a = []string{"TEST>HTAPRS,TCPIP*:>http packet content"}
	}

	c, err := dialTestServer(*server, *call, "post", 0, "")
	if err != nil {
		return err
	}
	c.conn.Close()

	body := loginLine(*call, "post", 0, "") + "\r\n" + strings.Join(a, "\r\n") + "\r\n"
	req, err := http.NewRequest("POST", *url, strings.NewReader(body))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/octet-stream")
	req.Header.Set("Accept-Type", "text/plain")

	// a dialer which only connects to a loopback address
	dialer := &net.Dialer{Timeout: 10 * time.Second}
	dialLoopback := func(ctx context.Context, network, addr string) (net.Conn, error) {
		conn, err := dialer.DialContext(ctx, network, addr)
		if err != nil {
			return nil, err
		}
		if err := checkLoopback(conn.RemoteAddr()); err != nil {
			conn.Close()
			return nil, err
		}
		return conn, nil
	}
	client := &http.Client{
		Timeout:       10 * time.Second,
		Transport:     &http.Transport{DialContext: dialLoopback},
		CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse },
	}
	resp, err := client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	reply, err := io.ReadAll(resp.Body)
	if err != nil {
		return err
	}
	fmt.Printf("%s\n\n%s\n", resp.Status, reply)
	return nil
}
