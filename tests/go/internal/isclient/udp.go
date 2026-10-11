package isclient

import (
	"net"
	"strconv"
	"strings"
	"testing"
	"time"
)

// maxDatagram is the largest UDP datagram the test sockets read.
const maxDatagram = 2048

// readDatagram reads one datagram within timeout, without a trailing CRLF.
func readDatagram(conn net.PacketConn, timeout time.Duration) (string, error) {
	if err := conn.SetReadDeadline(time.Now().Add(timeout)); err != nil {
		return "", err
	}

	buf := make([]byte, maxDatagram)
	n, _, err := conn.ReadFrom(buf)
	if err != nil {
		return "", err
	}

	return strings.TrimRight(string(buf[:n]), "\r\n"), nil
}

// UDPClient is a client which logs in over TCP and asks the server to send it
// packets over UDP.
type UDPClient struct {
	*Plain
	usock net.PacketConn
}

// ConnectUDP binds the UDP port, logs in to addr as callsign with that UDP
// port, and returns the client. Packets are received with ReadPacket from the
// UDP socket; the TCP connection only keeps the session up.
func ConnectUDP(t *testing.T, addr, callsign string, port int) *UDPClient {
	t.Helper()

	usock, err := net.ListenPacket("udp", net.JoinHostPort("", strconv.Itoa(port)))
	if err != nil {
		t.Fatalf("failed to bind UDP port %d: %v", port, err)
	}
	t.Cleanup(func() { usock.Close() })

	return &UDPClient{
		Plain: Connect(t, addr, callsign, withUDPPort(port)),
		usock: usock,
	}
}

// ReadPacket receives a packet from the UDP socket.
func (c *UDPClient) ReadPacket(timeout time.Duration) (string, error) {
	return readDatagram(c.usock, timeout)
}

// UDPPeer is a UDP socket which plays a core peer of the server under test:
// it sends datagrams to the server's peer group address and receives the ones
// the server sends to it.
type UDPPeer struct {
	conn *net.UDPConn
	dest *net.UDPAddr
}

// ListenUDPPeer binds the UDP address local, and sends to dest.
func ListenUDPPeer(t *testing.T, local, dest string) *UDPPeer {
	t.Helper()

	laddr, err := net.ResolveUDPAddr("udp", local)
	if err != nil {
		t.Fatalf("bad UDP address %q: %v", local, err)
	}
	daddr, err := net.ResolveUDPAddr("udp", dest)
	if err != nil {
		t.Fatalf("bad UDP address %q: %v", dest, err)
	}

	conn, err := net.ListenUDP("udp", laddr)
	if err != nil {
		t.Fatalf("failed to bind UDP socket %s: %v", local, err)
	}
	t.Cleanup(func() { conn.Close() })

	// small buffers, so that the load tests notice packets being lost
	conn.SetReadBuffer(32768)
	conn.SetWriteBuffer(32768)

	return &UDPPeer{conn: conn, dest: daddr}
}

// SendLine sends the line as one datagram.
func (p *UDPPeer) SendLine(line string) error {
	return p.SendRaw([]byte(line))
}

// SendRaw sends the bytes as one datagram.
func (p *UDPPeer) SendRaw(b []byte) error {
	_, err := p.conn.WriteToUDP(b, p.dest)
	return err
}

// ReadPacket receives one datagram.
func (p *UDPPeer) ReadPacket(timeout time.Duration) (string, error) {
	return readDatagram(p.conn, timeout)
}
