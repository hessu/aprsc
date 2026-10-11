# Functional tests

Functional tests for aprsc: each test starts the real `aprsc` binary with a
configuration from `../cfg-aprsc/`, connects fake clients, upstreams and UDP
peers to it, and checks what comes out. The suite was ported from an earlier
Perl suite in 2026; each test's header comment names the Perl test it came
from, for digging in the git history.

    cd src && ./configure && make       # build aprsc first
    make testinstall                    # copies the web files to tests/web
    cd ../tests && make test            # or: make tls-testcert; cd go && go test ./...

`make test-short`, `make test-race` and `make test-run T=TestDupecheck` are
variants (see `../Makefile`). All of them generate the TLS test CA and
certificates on the first run, which the TLS listeners of the `basic`
configuration need; `go test` run by hand needs `make tls-testcert` done in
`../` first.

The tests run a few at a time, three unless `-parallel n` (or `make test
PARALLEL=n`) says otherwise, each with a server of its own on ports of its
own. The configurations in `../cfg-aprsc/` use fixed port numbers, and the
tests refer to the same numbers through `harness.Port`, `harness.Addr` and
`harness.AddrOn`, never literally. The first test to run gets those very
ports, and the others get ranges from 20100 up, with the ports in their
copies of the configuration mapped accordingly; `PARALLEL=1` runs the tests
one at a time, all on the original ports. `internal/harness/ports.go` lists
every port the configurations use, and a port added to a configuration must
be added there. Nothing else (an old aprsc, for example) may be listening on
port 55501.

## Options

* `APRSC_BIN`: the aprsc binary to test, default `../../src/aprsc`.
* `-parallel n`, or `PARALLEL=n` with make: how many tests run at a time.
* `go test -short` skips the heavy `TestUDPLoad`.
* `TEST_PRODUCT=javap` or `javap4` runs the tests against javAPRSSrvr, if a
  copy is in `../javaprssrvr` (or `../javaprssrvr4`). Tests of aprsc features
  are skipped.
* Failed tests print the tail of the server's log.

## IS2

IS2 is not compiled into aprsc by default. The IS2 tests (`TestIS2*`,
`TestDupeClientIS2`) are skipped unless the binary under test was built with
`./configure --enable-is2`; `TestIS2Disabled` runs only on a binary built
without it. The IS2 client uses Go types generated from `../../src/aprsis2.proto`
(`internal/aprsis2`). After a schema change, regenerate them with
`go generate ./internal/aprsis2` (needs `protoc` and `protoc-gen-go`);
`TestGeneratedIsCurrent` fails if that was forgotten.

## SCTP

`TestSCTP` needs a Linux aprsc build with SCTP support, and is skipped if the
server rejects the `sctp` listener.
