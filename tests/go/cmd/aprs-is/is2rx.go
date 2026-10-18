package main

import (
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/hessu/aprsc/tests/go/internal/aprsis2"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

func runIS2Rx(args []string) error {
	fs := newFlags("is2rx", "<server:port>",
		"Logs in to an IS2 port of the server and prints the packets it sends, each\n"+
			"preceded by a comment line with the metadata the packet came with: the\n"+
			"time it entered the APRS-IS, the servers which received it, and the radio\n"+
			"reception details if the igate reported them.")
	user := fs.String("user", "", "`callsign` to log in as (required)")
	passcode := fs.String("passcode", "-1", "`passcode` to log in with")
	filter := fs.String("filter", "", "`filter` to set after logging in")
	sctp := fs.Bool("sctp", false, "connect over SCTP instead of TCP (Linux only)")
	a := parseArgs(fs, args, 1, 1)
	server := a[0]
	if *user == "" {
		return errors.New("-user is required")
	}

	opts := []isclient.Option{
		isclient.WithPasscode(*passcode),
		isclient.WithAppName("aprs-is-is2rx"),
		isclient.WithConnectRetry(0),
	}
	if *sctp {
		opts = append(opts, isclient.WithSCTP())
	}
	if *filter != "" {
		opts = append(opts, isclient.WithFilter(*filter))
	}

	c, err := isclient.DialIS2(server, *user, opts...)
	if c != nil {
		defer c.Close()
	}
	if err != nil {
		if c == nil {
			return err
		}
		// logged in, but the filter was not accepted
		fmt.Fprintf(os.Stderr, "%v\n", err)
	}
	fmt.Fprintln(os.Stderr, is2LoginInfo(c, server, *sctp, *user, *filter, err == nil))

	out := newOutput()
	defer out.flush()

	return printIS2(c, out)
}

// is2LoginInfo describes the connection and the login for standard error.
func is2LoginInfo(c *isclient.IS2, server string, sctp bool, user, filter string, filterOK bool) string {
	proto := "TCP"
	if sctp {
		proto = "SCTP"
	}
	sig := c.Signature
	s := fmt.Sprintf("connected to %s over %s: server %s (%s %s), logged in as %s, verified %s",
		server, proto, sig.GetUsername(), sig.GetAppName(), sig.GetAppVersion(),
		user, c.LoginReply.GetVerified())
	if filter != "" && filterOK {
		s += ", filter " + filter
	}
	return s
}

// printIS2 prints the packets received on c, with their metadata, until the
// connection closes. Other frames are printed as comments.
func printIS2(c *isclient.IS2, out *output) error {
	for {
		m, err := c.ReadMessage(time.Hour)
		if err != nil {
			if isclient.IsTimeout(err) {
				continue
			}
			if errors.Is(err, io.EOF) {
				return nil
			}
			return err
		}

		switch m.GetType() {
		case aprsis2.IS2Message_IS_PACKET:
			now := time.Now()
			ps := m.GetIsPacket()
			for i, p := range ps {
				out.line([]byte(packetMeta(p, now)), false, false)
				out.line(p.GetIsPacketData(), false, i == len(ps)-1)
			}
		case aprsis2.IS2Message_SERVER_SIGNATURE:
			// sent again after a live upgrade of the server
			sig := m.GetServerSignature()
			out.line([]byte(fmt.Sprintf("# server signature: %s (%s %s)",
				sig.GetUsername(), sig.GetAppName(), sig.GetAppVersion())), false, true)
		case aprsis2.IS2Message_PARAMETER:
			p := m.GetParameter()
			out.line([]byte(fmt.Sprintf("# parameter %s: filter %q",
				strings.TrimPrefix(p.GetType().String(), "PARAMETER_"), p.GetFilterString())), false, true)
		default:
			out.line([]byte("# frame "+m.GetType().String()), false, true)
		}
	}
}

// packetMeta formats the metadata of a packet as a comment line: the fields
// which are set, in the order of the schema.
func packetMeta(p *aprsis2.ISPacket, now time.Time) string {
	var f []string

	switch p.GetType() {
	case aprsis2.ISPacket_IS_PACKET:
	case aprsis2.ISPacket_IS_PACKET_DUPLICATE:
		f = append(f, "duplicate")
	default:
		f = append(f, "type="+p.GetType().String())
	}

	if t := p.GetReceivedTime(); t != 0 {
		rt := time.UnixMilli(int64(t)).UTC()
		f = append(f, "received_time="+rt.Format("2006-01-02T15:04:05.000Z"),
			fmt.Sprintf("age=%dms", now.Sub(rt).Milliseconds()))
	}

	if rcvd := p.GetReceived(); len(rcvd) > 0 {
		entries := make([]string, 0, len(rcvd))
		for _, r := range rcvd {
			entries = append(entries, receivedEntry(r))
		}
		f = append(f, "received=["+strings.Join(entries, "; ")+"]")
	}

	m := isclient.MetaOf(p)
	if m.RSSI != nil {
		f = append(f, fmt.Sprintf("rssi=%gdBm", *m.RSSI))
	}
	if m.SNR != nil {
		f = append(f, fmt.Sprintf("snr=%gdB", *m.SNR))
	}
	if m.Modem != aprsis2.ISPacket_MODEM_UNDEFINED {
		f = append(f, "modem="+strings.TrimPrefix(m.Modem.String(), "MODEM_"))
	}
	if m.Bitrate != nil {
		f = append(f, fmt.Sprintf("bitrate=%dbit/s", *m.Bitrate))
	}
	if m.Frequency != nil {
		f = append(f, "frequency="+strconv.FormatFloat(*m.Frequency/1e6, 'f', -1, 64)+"MHz")
	}

	if len(f) == 0 {
		return "# no metadata"
	}
	return "# " + strings.Join(f, " ")
}

// receivedEntry formats one Received entry: the server, who it received the
// packet from, how that client was verified and its address, if recorded.
func receivedEntry(r *aprsis2.IS2Received) string {
	s := fmt.Sprintf("%s from %s (%s", r.GetServerId(), r.GetUsername(), r.GetVerified())
	if addr := r.GetClientAddr(); len(addr) > 0 {
		if len(addr) == net.IPv4len || len(addr) == net.IPv6len {
			s += ", " + net.IP(addr).String()
		} else {
			s += ", addr 0x" + hex.EncodeToString(addr)
		}
	}
	return s + ")"
}
