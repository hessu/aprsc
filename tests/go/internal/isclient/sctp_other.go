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

// SCTPMsgConn is an SCTP connection which is read a message at a time. The
// SCTP client is only implemented on Linux.
type SCTPMsgConn struct {
	net.Conn
}

// DialSCTPMsg connects to addr over SCTP. The SCTP client is only implemented
// on Linux.
func DialSCTPMsg(addr string, timeout time.Duration) (*SCTPMsgConn, error) {
	return nil, ErrNoSCTP
}

// SetAckDelay turns on delayed acknowledgements. The SCTP client is only
// implemented on Linux.
func (c *SCTPMsgConn) SetAckDelay(d time.Duration) error {
	return ErrNoSCTP
}

// ReadMsg reads one SCTP message. The SCTP client is only implemented on
// Linux.
func (c *SCTPMsgConn) ReadMsg(timeout time.Duration) ([]byte, bool, error) {
	return nil, false, ErrNoSCTP
}
