package harness

import (
	"strconv"
	"strings"
	"testing"
)

func TestRewriteConfig(t *testing.T) {
	b := &portBlock{index: 1}
	p := func(base int) string {
		p, ok := b.port(base)
		if !ok {
			t.Fatalf("port %d not in the table", base)
		}
		return strconv.Itoa(p)
	}

	in := strings.Join([]string{
		`PassCode 55152`,
		`Listen "Full feed" fullfeed tcp ::0 55152 acl "cfg-aprsc/acl-all.acl" maxclients 1000 filter "r/60/25/1000"`,
		`Listen "Igate, TLS" igate tcp 0.0.0.0 \`,
		`	55580 tlskey cfg-aprsc/tls1-key.pem`,
		`Uplink full1 full tcp 127.0.0.1 10153`,
		`HTTPStatus 127.0.0.1 55501`,
		`PeerGroup TEST udp 127.0.0.1:16404 \`,
		`	PEER1 127.0.0.1:16405 \`,
		`	PEER2 [::1]:16406`,
		`#PeerGroup TEST6 udp [::1]:16504 \`,
		`#	PEER61 [::1]:16505`,
		`RunDir data`,
	}, "\n")
	want := strings.Join([]string{
		`PassCode 55152`,
		`Listen "Full feed" fullfeed tcp ::0 ` + p(55152) + ` acl "cfg-aprsc/acl-all.acl" maxclients 1000 filter "r/60/25/1000"`,
		`Listen "Igate, TLS" igate tcp 0.0.0.0 \`,
		`	` + p(55580) + ` tlskey cfg-aprsc/tls1-key.pem`,
		`Uplink full1 full tcp 127.0.0.1 ` + p(10153),
		`HTTPStatus 127.0.0.1 ` + p(55501),
		`PeerGroup TEST udp 127.0.0.1:` + p(16404) + ` \`,
		`	PEER1 127.0.0.1:` + p(16405) + ` \`,
		`	PEER2 [::1]:` + p(16406),
		`#PeerGroup TEST6 udp [::1]:16504 \`,
		`#	PEER61 [::1]:16505`,
		`RunDir /run/here`,
	}, "\n")

	got, err := b.rewriteConfig(in, "/run/here")
	if err != nil {
		t.Fatal(err)
	}
	if got != want {
		t.Errorf("rewriteConfig:\n got: %s\nwant: %s", got, want)
	}

	for _, bad := range []string{
		`Listen "Full feed" fullfeed tcp ::0 14580`,
		`PeerGroup TEST udp 127.0.0.1:16404 PEER1 127.0.0.1`,
		`HTTPStatus 127.0.0.1 port`,
	} {
		if _, err := b.rewriteConfig(bad, "/run/here"); err == nil {
			t.Errorf("no error for %q", bad)
		}
	}
}
