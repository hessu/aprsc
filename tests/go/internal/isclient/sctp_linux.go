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
	"time"
)

// The Go standard library has no SCTP, but a one-to-one style SCTP socket
// (SOCK_STREAM, IPPROTO_SCTP) is read and written like a TCP one, so a socket
// connected with system calls is wrapped in a net.Conn with net.FileConn.
// Every write is sent as one SCTP message.

const (
	ipprotoSCTP     = 132    // IPPROTO_SCTP
	sctpNodelay     = 3      // SCTP_NODELAY in <netinet/sctp.h>
	sctpDelayedSack = 16     // SCTP_DELAYED_SACK
	sctpRecvRcvinfo = 32     // SCTP_RECVRCVINFO
	sctpRcvinfo     = 3      // SCTP_RCVINFO, the type of the control message
	sctpUnordered   = 1      // SCTP_UNORDERED in the flags of struct sctp_rcvinfo
	msgNotification = 0x8000 // MSG_NOTIFICATION
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

func dialSCTPIP(ip net.IP, port int, timeout time.Duration) (net.Conn, error) {
	family := syscall.AF_INET6
	var sa syscall.Sockaddr
	if ip4 := ip.To4(); ip4 != nil {
		family = syscall.AF_INET
		sa4 := &syscall.SockaddrInet4{Port: port}
		copy(sa4.Addr[:], ip4)
		sa = sa4
	} else {
		sa6 := &syscall.SockaddrInet6{Port: port}
		copy(sa6.Addr[:], ip.To16())
		sa = sa6
	}

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
