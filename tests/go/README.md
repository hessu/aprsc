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
`harness.AddrOn`, never literally: each running test gets a block of ports
in the 20000 range, and the harness maps the numbers to it in the copy of
the configuration the server runs with. The blocks lie below the ephemeral
port range, where the numbers in the configurations would be at the mercy of
any outgoing connection. `internal/harness/ports.go` lists every port the
configurations use, and a port added to a configuration must be added there.

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
server rejects the `sctp` listener. The IS2 over SCTP tests (`TestIS2SCTP*`)
need IS2 too, and an SCTP client: the Go standard library has none, so
`internal/isclient` connects one with system calls (Linux only), selected with
the `WithSCTP` option. They are skipped if the kernel can not create SCTP
sockets.

## Performance scenarios

`perf/` has scenarios which measure rather than check, behind the `perf`
build tag so that the test suite does not run them: `make perftest` in
`../`, or `go test -tags perf -v ./perf/` here. `TestCorepeerCPU` feeds
unique packets to a server at a steady rate (200/s for 30 s by default),
which passes them to a second server over a UDP core peer link, plain or
IS2, to a full feed client of the second server, plaintext or IS2, and
reports the CPU use of both servers (user and system time, as the `time`
command shows them, from the resource usage the kernel reports for the
process) for each of the four combinations. `TestClientCPU` feeds one
server the same way and has a number of full feed clients (10 by default,
`-nclients`), all plaintext or all IS2, receive every packet, and reports
the CPU use of the server for each client type. `make perftest
PERF_ARGS="-duration 10s -rate 500 -peers is2 -clients tcp"` changes the
load and the scenarios, and `-server-cpus 0-3 -client-cpus 4-11` pins the
servers and the test process to separate cores with `taskset`, so that
the clients, which take several cores with a thousand of them, do not
compete with the servers being measured.

## APRS-IS client tools

`cmd/aprs-is` is a command for testing and loading a server by hand, which
replaced the Perl scripts that were in `../../tools/`. Build it with `go build
./cmd/aprs-is` here, and see `aprs-is <command> -h` for the options:

* `rx`: log in and print what the server sends; `-t` prefixes the lines with
  the Unix time, `-udp` asks for the packets over UDP, `-f` sets a filter.
* `is2rx`: log in to an IS2 port and print the packets, each preceded by a
  comment line with its metadata: the time it entered the APRS-IS, the
  Received entries, and the RSSI, SNR, modem, bit rate and frequency if the
  igate reported them. `-user` names the callsign, `-passcode` the passcode
  (`-1` unless given), `-filter` a filter, and `-sctp` connects over SCTP
  (Linux only). It can be used against a real server.
* `xmit`: send a few sample packets (or the lines of a file) slowly, and
  print what comes back.
* `feed`: send the packets read from standard input as fast as the server
  takes them. The input is in the format `rx -t` writes, so traffic recorded
  with it can be replayed; `-raw` takes plain packets.
* `multirx`: keep many receiving connections open.
* `coresim`: connect the 714 clients of the APRS-IS core servers of March
  2008 (`cmd/aprs-is/coresim.txt`) with their filters, and discard what they
  receive. `-map` maps their ports to the ports of the test server.
* `post`: send packets with an HTTP POST to the position upload port.

`multirx` and `coresim` print the number of connections and the receive
rate every 10 seconds.

Run them against a local test server with no uplinks, not against the real
APRS-IS; `is2rx` and `rx`, which only receive, are the exception. To load a
test server with real traffic, give it a read-only (`ro`) uplink to a real
server.
