# Go functional tests

Functional tests for aprsc: each test starts the real `aprsc` binary with a
configuration from `../cfg-aprsc/`, connects fake clients, upstreams and UDP
peers to it, and checks what comes out. This is a Go port of the Perl suite in
`../t/`; the Perl tests remain until the port has been run on Linux.

    cd src && ./configure && make       # build aprsc first
    make testinstall                    # copies the web files to tests/web
    cd ../tests && make gotest          # or: cd go && go test ./...

`make gotest-short`, `make gotest-race` and `make gotest-run T=TestDupecheck`
are variants (see `../Makefile`).

The tests use fixed ports, so they run one at a time, and nothing else (an old
aprsc, for example) may be listening on port 55501.

## Options

* `APRSC_BIN`: the aprsc binary to test, default `../../src/aprsc`.
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
