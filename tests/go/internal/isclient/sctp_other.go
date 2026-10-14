//go:build !linux

package isclient

import (
	"net"
	"time"
)

// SCTPSupported tells whether SCTP sockets can be created on this host. The
// SCTP client is only implemented on Linux.
func SCTPSupported() error {
	return ErrNoSCTP
}

// DialSCTP connects to addr over SCTP. The SCTP client is only implemented on
// Linux.
func DialSCTP(addr string, timeout time.Duration) (net.Conn, error) {
	return nil, ErrNoSCTP
}
