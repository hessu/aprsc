package isclient

import (
	"errors"
	"fmt"
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
	ipprotoSCTP = 132 // IPPROTO_SCTP
	sctpNodelay = 3   // SCTP_NODELAY in <netinet/sctp.h>
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

	// FileConn makes a copy of the descriptor
	f := os.NewFile(uintptr(fd), "sctp")
	defer f.Close()
	return net.FileConn(f)
}
