package functional

import (
	"fmt"
	"strings"
	"testing"
	"time"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// maxPacketLen is the longest packet, with the q construct and server
// additions, that the server under test forwards.
func maxPacketLen() int {
	if strings.HasPrefix(harness.ProductName(), "javap") {
		return 510
	}
	return 509
}

// Q constructs for packets from an uplink connection, over IPv4 and IPv6.
// Port of t/22qconstr-uplink.t.
func TestQConstrUplink(t *testing.T) {
	const upstreamCall = "FAKEUP"

	fake4 := isclient.ListenFakeUpstream(t, "127.0.0.1:54153", upstreamCall)

	s := harness.Start(t, "uplinks")

	up1 := fake4.Accept(t)
	rx := isclient.Connect(t, "127.0.0.1:55152", "N5CAL-1")

	// If the q construct is qAI and the packet is from an outbound
	// connection, append ,IPADDR (here the upstream's callsign), then
	// ,SERVERLOGIN.
	t.Run("ipv4", func(t *testing.T) {
		isclient.RunSteps(t, up1, rx, []isclient.Step{
			pass("qAI from uplink",
				"SRC>DST,DIGI1,DIGI2*,qAI,FOOBA,BLAA:testing qAI (1)",
				"SRC>DST,DIGI1,DIGI2*,qAI,FOOBA,BLAA,"+upstreamCall+","+serverCall+":testing qAI (1)"),
		})
	})

	// unbind the IPv4 server and create an IPv6 server
	fake4.Close()
	fake6 := isclient.ListenFakeUpstream(t, "[::]:54153", upstreamCall)
	up1.Close()
	up6 := fake6.Accept(t)

	t.Run("ipv6", func(t *testing.T) {
		isclient.RunSteps(t, up6, rx, []isclient.Step{
			pass("qAI from uplink",
				"SRC>DST,DIGI1,DIGI2*,qAI,FOOBAR,BLAA:testing qAI (ipv6)",
				"SRC>DST,DIGI1,DIGI2*,qAI,FOOBAR,BLAA,"+upstreamCall+","+serverCall+":testing qAI (ipv6)"),
		})
	})

	s.Stop()
}

// qPathPackets are packets with a q path of every length from 0 to 70
// elements, in a few shapes.
func qPathPackets() []string {
	var packets []string

	for i := 1; i < 500; i++ {
		elems := i / 7
		extra := i % 7

		var l []string
		for d := 0; d < elems; d++ {
			l = append(l, fmt.Sprintf("SRV%03d", d))
		}
		if extra != 0 {
			l = append(l, strings.Repeat("D", extra))
		} else if len(l) > 0 {
			last := l[len(l)-1]
			l[len(l)-1] = last[:len(last)-1] + ",Y"
		}

		p := "SRC>DST,qAI," + strings.Join(l, ",") + fmt.Sprintf(":packet %d", i)
		packets = append(packets, strings.ReplaceAll(p, ",,", ","))
	}

	return packets
}

// digiPackets are packets with a digipeater path of every length from 0 to 70
// elements, ending in a q construct.
func digiPackets() []string {
	var packets []string

	for i := 0; i < 500; i++ {
		elems := i / 7
		extra := i % 7

		var l []string
		for d := 0; d < elems; d++ {
			l = append(l, fmt.Sprintf("DIG%03d", d))
		}
		if extra != 0 {
			l = append(l, strings.Repeat("D", extra))
		} else if len(l) > 0 {
			l[len(l)-1] += "*"
		}

		p := "SRC>DST," + strings.Join(l, ",") + fmt.Sprintf(",qAI,IGATE:packet %d", i)
		packets = append(packets, strings.Replace(p, ",,", ",", 1))
	}

	return packets
}

// sendUplinkPackets sends the packets from the upstream connection. The
// server appends the upstream's call and its own to the packet's q path with
// the expected packet being what the append function makes of it. Packets
// which would become too long are only sent; the server drops them, which is
// checked by the silence at the end.
func sendUplinkPackets(t *testing.T, up *isclient.Peer, rx *isclient.Plain, packets []string, expected func(string) string) {
	t.Helper()

	maxLen := maxPacketLen()

	for i, p := range packets {
		want := expected(p)
		if len(want) > maxLen {
			if err := up.SendLine(p); err != nil {
				t.Fatalf("write to server failed: %v", err)
			}
			continue
		}

		isclient.TxRx(t, up, rx, p, want)
		if t.Failed() {
			t.Fatalf("stopping after the first failure, at packet %d", i)
		}
	}

	isclient.ExpectSilence(t, up, time.Second, "read-only upstream connection")
	isclient.ExpectSilence(t, rx, time.Second, "full stream")
}

// Long q constructs from an uplink: a q path of every length, up to and past
// the maximum packet length. Port of t/23qconstr-long.t.
func TestQConstrLong(t *testing.T) {
	fake6 := isclient.ListenFakeUpstream(t, "[::]:54153", "FAKEIS")

	s := harness.Start(t, "uplinks")

	up := fake6.Accept(t)
	rx := isclient.Connect(t, "localhost:55152", "N5CAL-1")

	sendUplinkPackets(t, up, rx, qPathPackets(), func(p string) string {
		return strings.Replace(p, ":", ",FAKEIS,"+serverCall+":", 1)
	})

	s.Stop()
}

// Long digipeater paths from an uplink, up to and past the maximum packet
// length. Port of t/27digipath-long.t.
func TestDigipathLong(t *testing.T) {
	fake6 := isclient.ListenFakeUpstream(t, "[::]:54153", "FAKE6")

	s := harness.Start(t, "uplinks")

	up := fake6.Accept(t)
	rx := isclient.Connect(t, "localhost:55152", "N5CAL-1")

	sendUplinkPackets(t, up, rx, digiPackets(), func(p string) string {
		return strings.Replace(p, "IGATE:", "IGATE,FAKE6,"+serverCall+":", 1)
	})

	s.Stop()
}
