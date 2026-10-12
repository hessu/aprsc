package main

import (
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"
)

func runPost(args []string) error {
	fs := newFlags("post", "[packet ...]",
		"Sends packets to the server's HTTP position upload port in one POST, and\n"+
			"prints the reply. Without arguments, one test packet is sent.")
	url := fs.String("url", "http://127.0.0.1:55080/", "the `URL` to post to")
	call := fs.String("c", "TEST", "`callsign` to log in with")
	a := parseArgs(fs, args, 0, 1<<20)
	if len(a) == 0 {
		a = []string{"TEST>HTAPRS,TCPIP*:>http packet content"}
	}

	body := loginLine(*call, "post", 0, "") + "\r\n" + strings.Join(a, "\r\n") + "\r\n"
	req, err := http.NewRequest("POST", *url, strings.NewReader(body))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/octet-stream")
	req.Header.Set("Accept-Type", "text/plain")

	client := &http.Client{
		Timeout:       10 * time.Second,
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
