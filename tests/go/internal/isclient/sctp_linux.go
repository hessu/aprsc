package isclient

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"strconv"
	"syscall"
	"testing"
	"time"
	"unsafe"
)

// The Go standard library has no SCTP, but a one-to-one style SCTP socket
// (SOCK_STREAM, IPPROTO_SCTP) is read and written like a TCP one, so a socket
// connected with system calls is wrapped in a net.Conn with net.FileConn.
// Every write is sent as one SCTP message.

const (
	ipprotoSCTP      = 132    // IPPROTO_SCTP
	sctpNodelay      = 3      // SCTP_NODELAY in <netinet/sctp.h>
	sctpDelayedSack  = 16     // SCTP_DELAYED_SACK
	sctpRecvRcvinfo  = 32     // SCTP_RECVRCVINFO
	sctpRcvinfo      = 3      // SCTP_RCVINFO, the type of the control message
	sctpGetPeerAddrs = 108    // SCTP_GET_PEER_ADDRS
	sctpUnordered    = 1      // SCTP_UNORDERED in the flags of struct sctp_rcvinfo
	msgNotification  = 0x8000 // MSG_NOTIFICATION
)

// SCTPSupported tells whether SCTP sockets can be created on this host: the
// kernel may lack SCTP support, or have the module blacklisted.
func SCTPSupported() error {
	fd, err := syscall.Socket(syscall.AF_INET, syscall.SOCK_STREAM|syscall.SOCK_CLOEXEC, ipprotoSCTP)
	if err != nil {
		return fmt.Errorf("%w: %v", ErrNoSCTP, err)
	}
	syscall.Close(fd)
	return nil
}

// DialSCTP connects to addr ("host:port") over SCTP. The addresses of the
// host are tried in turn, each for at most timeout.
func DialSCTP(addr string, timeout time.Duration) (net.Conn, error) {
	host, portS, err := net.SplitHostPort(addr)
	if err != nil {
		return nil, err
	}
	port, err := strconv.Atoi(portS)
	if err != nil {
		return nil, fmt.Errorf("bad port in %q: %w", addr, err)
	}
	ips, err := net.LookupIP(host)
	if err != nil {
		return nil, err
	}

	var errs []error
	for _, ip := range ips {
		c, err := dialSCTPIP(ip, port, timeout)
		if err == nil {
			return c, nil
		}
		errs = append(errs, fmt.Errorf("%s: %w", net.JoinHostPort(ip.String(), portS), err))
	}
	return nil, fmt.Errorf("SCTP connect to %s failed: %w", addr, errors.Join(errs...))
}

// sctpSockaddr returns the socket family of ip and the sockaddr of ip and
// port.
func sctpSockaddr(ip net.IP, port int) (int, syscall.Sockaddr) {
	if ip4 := ip.To4(); ip4 != nil {
		sa4 := &syscall.SockaddrInet4{Port: port}
		copy(sa4.Addr[:], ip4)
		return syscall.AF_INET, sa4
	}
	sa6 := &syscall.SockaddrInet6{Port: port}
	copy(sa6.Addr[:], ip.To16())
	return syscall.AF_INET6, sa6
}

func dialSCTPIP(ip net.IP, port int, timeout time.Duration) (net.Conn, error) {
	family, sa := sctpSockaddr(ip, port)

	fd, err := syscall.Socket(family, syscall.SOCK_STREAM|syscall.SOCK_CLOEXEC, ipprotoSCTP)
	if err != nil {
		return nil, fmt.Errorf("%w: %v", ErrNoSCTP, err)
	}

	// a blocking connect, limited by the send timeout
	tv := syscall.NsecToTimeval(timeout.Nanoseconds())
	if err := syscall.SetsockoptTimeval(fd, syscall.SOL_SOCKET, syscall.SO_SNDTIMEO, &tv); err != nil {
		syscall.Close(fd)
		return nil, err
	}
	for {
		err = syscall.Connect(fd, sa)
		if err != syscall.EINTR {
			break
		}
	}
	if err != nil {
		syscall.Close(fd)
		return nil, err
	}
	tv = syscall.Timeval{}
	if err := syscall.SetsockoptTimeval(fd, syscall.SOL_SOCKET, syscall.SO_SNDTIMEO, &tv); err != nil {
		syscall.Close(fd)
		return nil, err
	}
	// send each write right away, like the server does
	if err := syscall.SetsockoptInt(fd, ipprotoSCTP, sctpNodelay, 1); err != nil {
		syscall.Close(fd)
		return nil, err
	}
	// Acknowledge each message right away. The server starts the data
	// after the login when the client has acknowledged the login reply,
	// which would otherwise take the 200 ms of a delayed acknowledgement,
	// and a packet sent right after the login would not be passed to the
	// client. struct sctp_sack_info: the association, the delay (0 leaves
	// it unchanged) and the frequency (1 turns off delayed acks).
	var sackInfo [12]byte
	binary.NativeEndian.PutUint32(sackInfo[8:], 1)
	if err := syscall.SetsockoptString(fd, ipprotoSCTP, sctpDelayedSack, string(sackInfo[:])); err != nil {
		syscall.Close(fd)
		return nil, fmt.Errorf("setsockopt SCTP_DELAYED_SACK: %w", err)
	}

	// FileConn makes a copy of the descriptor
	f := os.NewFile(uintptr(fd), "sctp")
	defer f.Close()
	return net.FileConn(f)
}

// SCTPMsgConn is an SCTP connection which is read a message at a time, and
// tells how each message was sent.
type SCTPMsgConn struct {
	net.Conn
	rc syscall.RawConn
}

// DialSCTPMsg connects to addr over SCTP, like DialSCTP, for reading the
// messages with ReadMsg.
func DialSCTPMsg(addr string, timeout time.Duration) (*SCTPMsgConn, error) {
	c, err := DialSCTP(addr, timeout)
	if err != nil {
		return nil, err
	}
	sc, ok := c.(syscall.Conn)
	if !ok {
		c.Close()
		return nil, fmt.Errorf("SCTP connection %T has no file descriptor", c)
	}
	rc, err := sc.SyscallConn()
	if err != nil {
		c.Close()
		return nil, err
	}
	// receive a struct sctp_rcvinfo with each message
	var serr error
	err = rc.Control(func(fd uintptr) {
		serr = syscall.SetsockoptInt(int(fd), ipprotoSCTP, sctpRecvRcvinfo, 1)
	})
	if err == nil {
		err = serr
	}
	if err != nil {
		c.Close()
		return nil, fmt.Errorf("setsockopt SCTP_RECVRCVINFO: %w", err)
	}
	return &SCTPMsgConn{Conn: c, rc: rc}, nil
}

// SetAckDelay turns on delayed acknowledgements, which DialSCTP turns off,
// with a delay of d (at most 500 ms).
func (c *SCTPMsgConn) SetAckDelay(d time.Duration) error {
	var sackInfo [12]byte
	binary.NativeEndian.PutUint32(sackInfo[4:], uint32(d.Milliseconds()))
	binary.NativeEndian.PutUint32(sackInfo[8:], 2)
	var serr error
	err := c.rc.Control(func(fd uintptr) {
		serr = syscall.SetsockoptString(int(fd), ipprotoSCTP, sctpDelayedSack, string(sackInfo[:]))
	})
	if err == nil {
		err = serr
	}
	if err != nil {
		return fmt.Errorf("setsockopt SCTP_DELAYED_SACK: %w", err)
	}
	return nil
}

// ReadMsg reads one SCTP message, of up to 64 kB, and tells whether it was
// sent unordered.
func (c *SCTPMsgConn) ReadMsg(timeout time.Duration) (msg []byte, unordered bool, err error) {
	if err := c.SetReadDeadline(time.Now().Add(timeout)); err != nil {
		return nil, false, err
	}
	buf := make([]byte, 65536)
	oob := make([]byte, 256)
	for {
		var n, oobn, flags int
		var rerr error
		err := c.rc.Read(func(fd uintptr) bool {
			n, oobn, flags, _, rerr = syscall.Recvmsg(int(fd), buf, oob, 0)
			return rerr != syscall.EAGAIN
		})
		if err != nil {
			return nil, false, err
		}
		if rerr != nil {
			return nil, false, rerr
		}
		if n == 0 {
			return nil, false, io.EOF
		}
		if flags&msgNotification != 0 {
			continue
		}
		if flags&syscall.MSG_EOR == 0 {
			return nil, false, fmt.Errorf("SCTP message of over %d bytes", len(buf))
		}
		cmsgs, err := syscall.ParseSocketControlMessage(oob[:oobn])
		if err != nil {
			return nil, false, err
		}
		for _, m := range cmsgs {
			// struct sctp_rcvinfo starts with the stream, the stream
			// sequence number and the flags, 16 bits each
			if m.Header.Level == ipprotoSCTP && m.Header.Type == sctpRcvinfo && len(m.Data) >= 6 {
				return buf[:n], binary.NativeEndian.Uint16(m.Data[4:6])&sctpUnordered != 0, nil
			}
		}
		return nil, false, errors.New("no SCTP_RCVINFO received with the message")
	}
}

// ListenFakeUpstreamSCTP is ListenFakeUpstream over SCTP. addr is an IP
// address and a port, like "127.0.0.1:54153"; "[::]:54153" listens on all
// addresses of both families.
func ListenFakeUpstreamSCTP(t *testing.T, addr, call string) *FakeUpstream {
	t.Helper()

	host, portS, err := net.SplitHostPort(addr)
	if err != nil {
		t.Fatal(err)
	}
	port, err := strconv.Atoi(portS)
	if err != nil {
		t.Fatalf("bad port in %q: %v", addr, err)
	}
	ip := net.ParseIP(host)
	if ip == nil {
		t.Fatalf("bad IP address in %q", addr)
	}
	family, sa := sctpSockaddr(ip, port)

	fd, err := syscall.Socket(family, syscall.SOCK_STREAM|syscall.SOCK_CLOEXEC, ipprotoSCTP)
	if err != nil {
		t.Fatalf("failed to create an SCTP socket: %v", err)
	}
	if err := syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1); err != nil {
		syscall.Close(fd)
		t.Fatalf("setsockopt SO_REUSEADDR: %v", err)
	}
	if family == syscall.AF_INET6 {
		if err := syscall.SetsockoptInt(fd, syscall.IPPROTO_IPV6, syscall.IPV6_V6ONLY, 0); err != nil {
			syscall.Close(fd)
			t.Fatalf("setsockopt IPV6_V6ONLY: %v", err)
		}
	}
	if err := syscall.Bind(fd, sa); err != nil {
		syscall.Close(fd)
		t.Fatalf("failed to bind SCTP socket to %s: %v", addr, err)
	}
	if err := syscall.Listen(fd, 8); err != nil {
		syscall.Close(fd)
		t.Fatalf("failed to listen on %s: %v", addr, err)
	}

	// FileListener makes a copy of the descriptor. A one-to-one style
	// SCTP socket is a stream socket, so it comes out as a TCP listener.
	f := os.NewFile(uintptr(fd), "sctp")
	defer f.Close()
	ln, err := net.FileListener(f)
	if err != nil {
		t.Fatalf("failed to listen on %s: %v", addr, err)
	}
	t.Cleanup(func() { ln.Close() })
	tl, ok := ln.(*net.TCPListener)
	if !ok {
		t.Fatalf("SCTP listener is a %T, not a TCP listener", ln)
	}

	return &FakeUpstream{ln: tl, call: call}
}

// PeerAddrs lists the addresses of the peer of an SCTP connection, as the
// association knows them (SCTP_GET_PEER_ADDRS): the ones it is multi-homed
// on. The port is left out.
func (p *Peer) PeerAddrs() ([]string, error) {
	sc, ok := p.conn.(syscall.Conn)
	if !ok {
		return nil, fmt.Errorf("connection %T has no file descriptor", p.conn)
	}
	rc, err := sc.SyscallConn()
	if err != nil {
		return nil, err
	}
	// struct sctp_getaddrs: the association id (ignored on a one-to-one
	// style socket), the number of addresses, and the addresses packed
	// one after another, each a sockaddr_in or a sockaddr_in6
	b := make([]byte, 1024)
	blen := uint32(len(b))
	var serr error
	err = rc.Control(func(fd uintptr) {
		_, _, errno := syscall.Syscall6(syscall.SYS_GETSOCKOPT, fd, uintptr(ipprotoSCTP), uintptr(sctpGetPeerAddrs),
			uintptr(unsafe.Pointer(&b[0])), uintptr(unsafe.Pointer(&blen)), 0)
		if errno != 0 {
			serr = errno
		}
	})
	if err == nil {
		err = serr
	}
	if err != nil {
		return nil, fmt.Errorf("getsockopt SCTP_GET_PEER_ADDRS: %w", err)
	}
	if int(blen) > len(b) {
		return nil, fmt.Errorf("SCTP_GET_PEER_ADDRS returned %d bytes", blen)
	}
	b = b[:blen]
	if len(b) < 8 {
		return nil, fmt.Errorf("SCTP_GET_PEER_ADDRS returned %d bytes", len(b))
	}
	n := int(binary.NativeEndian.Uint32(b[4:8]))
	b = b[8:]

	var addrs []string
	for i := 0; i < n; i++ {
		if len(b) < 2 {
			return nil, fmt.Errorf("SCTP_GET_PEER_ADDRS: address %d of %d is missing", i+1, n)
		}
		var ip net.IP
		var size int
		switch binary.NativeEndian.Uint16(b[0:2]) {
		case syscall.AF_INET:
			size = 16 // sockaddr_in
			if len(b) >= size {
				ip = net.IP(b[4:8])
			}
		case syscall.AF_INET6:
			size = 28 // sockaddr_in6
			if len(b) >= size {
				ip = net.IP(b[8:24])
			}
		default:
			return nil, fmt.Errorf("SCTP_GET_PEER_ADDRS: address %d has unknown family %d", i+1, binary.NativeEndian.Uint16(b[0:2]))
		}
		if ip == nil {
			return nil, fmt.Errorf("SCTP_GET_PEER_ADDRS: address %d of %d is truncated", i+1, n)
		}
		addrs = append(addrs, ip.String())
		b = b[size:]
	}
	return addrs, nil
}
