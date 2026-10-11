
# Makefile for running the test suite

# The test suite, in go/, needs the aprsc binary built in ../src and "make
# testinstall" done there, and the TLS test CA and certificates, which are
# generated on the first run: the basic configuration has TLS listeners, and
# aprsc does not start without their certificates. Set APRSC_BIN to test
# another binary, for example one built with ./configure --enable-is2: the IS2
# tests are skipped without it. PARALLEL is how many tests run at a time, each
# with a server of its own on ports of its own; PARALLEL=1 runs them one at a
# time.
GO=go
PARALLEL=3
GOTEST_FLAGS = -count=1 -timeout 20m -parallel $(PARALLEL)

test test-short test-race test-run: tls-testcert

# all the tests, including aggregator and live upgrade
test:
	cd go && $(GO) test $(GOTEST_FLAGS) ./...

# skipping the heavy UDP load test
test-short:
	cd go && $(GO) test $(GOTEST_FLAGS) -short ./...

# with the race detector, to check the test code itself
test-race:
	cd go && $(GO) test $(GOTEST_FLAGS) -race ./...

# one test or a group, for example: make test-run T=TestDupecheck
test-run:
	cd go && $(GO) test $(GOTEST_FLAGS) -v -run '$(T)' ./...

# The performance scenarios in go/perf, which are not part of the test suite.
# PERF_ARGS passes their flags, for example PERF_ARGS="-duration 10s -rate 500".
perftest:
	cd go && $(GO) test -tags perf -count=1 -timeout 30m -v ./perf/ -args $(PERF_ARGS)

.PHONY: test test-short test-race test-run perftest

tls-testca: tls-testca/cacert.pem
tls-testcert: tls-testca cfg-aprsc/tls1-cert.pem cfg-aprsc/tls-client-cert.pem

tls-testca/serial:
	mkdir -p tls-testca tmp
	(cd tls-testca && mkdir -p certs newcerts private)
	touch tls-testca/index.txt tls-testca/index.txt.attr
	echo 00 > tls-testca/serial

tls-testca/cacert.pem: tls-testca/serial
	openssl req -x509 -new -nodes -newkey rsa:2048 -keyout tls-testca/private/cakey.pem -sha256 -days 7300 \
		-subj "/O=aprsc TEST/OU=Testing Department/CN=Testing CA" -out tls-testca/cacert.pem

cfg-aprsc/tls1-cert.pem: tls-testca/cacert.pem
	openssl req -new -config tls-openssl.conf \
		-keyout cfg-aprsc/tls1-key.pem -out tmp/tls1-req.pem \
		-newkey rsa:2048 -nodes -sha256 \
		-subj "/O=aprsc TEST/callSign=TLS1/CN=tls1host.example.com"

	openssl ca -config tls-openssl.conf -batch \
		-in tmp/tls1-req.pem -out cfg-aprsc/tls1-cert.pem \
		-days 3650

cfg-aprsc/tls-client-cert.pem: tls-testca/cacert.pem
	openssl req -new -config tls-openssl.conf \
		-keyout cfg-aprsc/tls-client-key.pem -out tmp/tls-client-req.pem \
		-newkey rsa:2048 -nodes -sha256 \
		-subj "/O=aprsc TEST/callSign=N5CAL"

	openssl ca -config tls-openssl.conf -batch \
		-in tmp/tls-client-req.pem -out cfg-aprsc/tls-client-cert.pem \
		-days 3650

