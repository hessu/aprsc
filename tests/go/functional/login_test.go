package functional

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/asn1"
	"fmt"
	"math/big"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// Log in to the full feed port with a valid passcode. Port of t/01login.t.
func TestLogin(t *testing.T) {
	harness.Parallel(t)
	s := harness.Start(t, "basic")

	c := isclient.Connect(t, harness.Addr(t, 55152), "OH7LZB")

	if !c.LogResp.Verified {
		t.Errorf("login was not verified: %q", c.LogResp.Raw)
	}
	if c.LogResp.Callsign != "OH7LZB" || c.LogResp.Server != "TESTING" {
		t.Errorf("unexpected logresp: %q", c.LogResp.Raw)
	}
	if harness.IsAprsc() && !strings.HasPrefix(c.Banner(), "# aprsc ") {
		t.Errorf("unexpected banner: %q", c.Banner())
	}

	if err := c.Close(); err != nil {
		t.Errorf("failed to disconnect: %v", err)
	}

	s.Stop()
}

// Logins which must be rejected: usernames on the built-in list and those
// matching the DisallowLoginCall patterns of the configuration, and login
// lines with no version number after the software name. Port of
// t/02login-reject.t.
func TestLoginReject(t *testing.T) {
	harness.Parallel(t)
	s := harness.Start(t, "basic")

	// expectReject sends a login line and expects the server to refuse it,
	// with the reply want on aprsc.
	expectReject := func(t *testing.T, login, want string) {
		t.Helper()

		reply, accepted := isclient.LoginReply(t, harness.Addr(t, 55152), login)
		if accepted {
			t.Errorf("login %q was accepted: %q", login, reply)
			return
		}
		if harness.IsAprsc() && reply != want {
			t.Errorf("login %q was refused with %q, want %q", login, reply, want)
		}
	}

	// "pass" is on the built-in list. The others match the configured
	// patterns "LOGINA LOGINB *prrej mi*rej sufre*", regardless of case.
	for _, call := range []string{"pass", "LOGINA", "loginb", "0prrej", "mi0rej", "sufrej"} {
		t.Run(call, func(t *testing.T) {
			expectReject(t,
				fmt.Sprintf("user %s pass %s vers aprsc-gotest 1.0", call, isclient.Passcode(call)),
				"# Login by user not allowed")
		})
	}

	// A client which sends "vers AppName filter ..." without a version number
	// between the software name and the filter keyword, and one which sends
	// nothing at all after the software name.
	const noVersion = "# Invalid login: software name and version are not separated by a space"
	t.Run("filter as version number", func(t *testing.T) {
		expectReject(t, "user OH7LZB pass -1 vers MyApp filter r/0/0/0", noVersion)
	})
	t.Run("no version number", func(t *testing.T) {
		expectReject(t, "user OH7LZB pass -1 vers MyApp", noVersion)
	})

	s.Stop()
}

// tlsClientConfig returns a TLS configuration for connecting to the test
// server's TLS listeners. The server's certificate is from the test CA,
// which the client does not trust, so verification is skipped. If a client
// certificate is given, it is sent whenever the server asks for one, even if
// the server would not accept its issuer.
func tlsClientConfig(cert *tls.Certificate) *tls.Config {
	c := &tls.Config{InsecureSkipVerify: true}
	if cert != nil {
		c.GetClientCertificate = func(*tls.CertificateRequestInfo) (*tls.Certificate, error) {
			return cert, nil
		}
	}
	return c
}

// callsignOID is the subject attribute carrying the amateur radio callsign
// in a LoTW certificate, which the server reads the callsign from.
var callsignOID = asn1.ObjectIdentifier{1, 3, 6, 1, 4, 1, 12348, 1, 1}

// selfSignedCert makes a certificate with the given callsign, signed by
// itself rather than by the test CA.
func selfSignedCert(t *testing.T, callsign string) *tls.Certificate {
	t.Helper()

	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	tmpl := &x509.Certificate{
		SerialNumber: big.NewInt(1),
		Subject: pkix.Name{
			Organization: []string{"aprsc TEST"},
			ExtraNames:   []pkix.AttributeTypeAndValue{{Type: callsignOID, Value: callsign}},
		},
		NotBefore:   time.Now().Add(-time.Hour),
		NotAfter:    time.Now().Add(time.Hour),
		KeyUsage:    x509.KeyUsageDigitalSignature,
		ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth},
	}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	if err != nil {
		t.Fatal(err)
	}
	return &tls.Certificate{Certificate: [][]byte{der}, PrivateKey: key}
}

// Logins to a TLS listener which validates client certificates, only on
// aprsc. A certificate from the test CA with the callsign of the login
// validates the login without a passcode; a missing certificate, one with
// another callsign, and one not issued by the CA are refused with a comment
// line, after which the server closes the connection. The server stays up
// through the refusals: the reply to a refused certificate used to be
// followed by writes to the client when the reply could not be written and
// the client had been closed and freed already.
func TestLoginTLSClientCert(t *testing.T) {
	harness.Parallel(t)
	harness.RequireAprsc(t)
	s := harness.Start(t, "basic")

	// the client certificate the test suite generates, with callsign N5CAL
	certDir := filepath.Join(harness.TestsDir(), "cfg-aprsc")
	caCert, err := tls.LoadX509KeyPair(filepath.Join(certDir, "tls-client-cert.pem"), filepath.Join(certDir, "tls-client-key.pem"))
	if err != nil {
		t.Fatalf("failed to load the test client certificate: %v", err)
	}

	addr := harness.Addr(t, 55582)
	const loginLine = "user %s pass -1 vers aprsc-gotest 1.0"

	// The callsign of the certificate, with or without an SSID, logs in
	// as verified without a passcode.
	for _, call := range []string{"N5CAL", "N5CAL-7"} {
		t.Run("certificate login "+call, func(t *testing.T) {
			c := isclient.Connect(t, addr, call, isclient.WithPasscode("-1"), isclient.WithTLS(tlsClientConfig(&caCert)))
			if !c.LogResp.Verified || c.LogResp.Callsign != call {
				t.Errorf("login with a client certificate was not verified: %q", c.LogResp.Raw)
			}
		})
	}

	expectReject := func(t *testing.T, call string, cert *tls.Certificate, want string) {
		t.Helper()
		login := fmt.Sprintf(loginLine, call)
		reply, accepted := isclient.LoginReply(t, addr, login, isclient.WithTLS(tlsClientConfig(cert)))
		if accepted {
			t.Errorf("login %q was accepted: %q", login, reply)
			return
		}
		if !strings.HasPrefix(reply, want) {
			t.Errorf("login %q was refused with %q, want %q", login, reply, want)
		}
	}

	t.Run("no certificate", func(t *testing.T) {
		expectReject(t, "N5CAL", nil, "# Client certificate authentication failed: Peer did not present a certificate")
	})
	t.Run("callsign mismatch", func(t *testing.T) {
		expectReject(t, "N0CALL", &caCert, "# Client certificate authentication failed: Certificate callsign does not match login username")
	})
	t.Run("untrusted certificate", func(t *testing.T) {
		expectReject(t, "N5CAL", selfSignedCert(t, "N5CAL"), "# Client certificate not accepted: ")
	})

	// the server is still there
	c := isclient.Connect(t, harness.Addr(t, 55580), "N5CAL-2")
	if !c.LogResp.Verified {
		t.Errorf("login after the refusals was not verified: %q", c.LogResp.Raw)
	}

	s.Stop()
}
