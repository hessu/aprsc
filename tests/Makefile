
# Makefile for running the test suite

TEST_FILES = t/*.t
LIGHT_TEST_IGNORE = t/64udp-load.t
LIGHT_TEST_FILES = $(filter-out $(LIGHT_TEST_IGNORE), $(wildcard t/*.t))

TEST_VERBOSE = 0
INST_LIB = libperl
INST_ARCHLIB = libperl

PERL=/usr/bin/perl

TESTID=`echo $$$$_$$RANDOM$$RANDOM$$RANDOM`
TEST_SRCCALLS="XX7FIT-1,XX7FIT-2"
TEST_IGATE="XX7IG"

test: tls-testcert
	@rm -f logs/aprsc.log
	PERL_DL_NONLAZY=1 $(PERL) "-MExtUtils::Command::MM" "-e" "test_harness($(TEST_VERBOSE), '$(INST_LIB)', '$(INST_ARCHLIB)')" $(TEST_FILES)

travistest:
	@rm -f logs/aprsc.log
	PERL_DL_NONLAZY=1 $(PERL) "-MExtUtils::Command::MM" "-e" "test_harness($(TEST_VERBOSE), '$(INST_LIB)', '$(INST_ARCHLIB)')" $(LIGHT_TEST_FILES)

AGGREGATOR_FILES = aggregator/*.t

aggrtest:
	@rm -f logs/aprsc.log
	TEST_ID=$(TESTID) TEST_IGATE=$(TEST_IGATE) TEST_SRCCALLS=$(TEST_SRCCALLS) \
	PERL_DL_NONLAZY=1 $(PERL) "-MExtUtils::Command::MM" "-e" "test_harness($(TEST_VERBOSE), '$(INST_LIB)', '$(INST_ARCHLIB)')" $(AGGREGATOR_FILES)

tls-testca: tls-testca/cacert.pem
tls-testcert: tls-testca cfg-aprsc/tls1-cert.pem

tls-testca/serial:
	mkdir -p tls-testca tmp
	(cd tls-testca && mkdir -p certs newcerts private)
	touch tls-testca/index.txt tls-testca/index.txt.attr
	echo 00 > tls-testca/serial

tls-testca/cacert.pem: tls-testca/serial
	openssl req -x509 -new -nodes -newkey rsa:2048 -keyout tls-testca/private/cakey.pem -sha256 -days 7300 \
		-subj "/O=aprsc TEST/OU=Testing Department/CN=Testing CA" -out tls-testca/cacert.pem

cfg-aprsc/tls1-cert.pem:
	openssl req -new -config tls-openssl.conf \
		-keyout cfg-aprsc/tls1-key.pem -out tmp/tls1-req.pem \
		-newkey rsa:2048 -nodes -sha256 \
		-subj "/O=aprsc TEST/callSign=TLS1/CN=tls1host.example.com"

	openssl ca -config tls-openssl.conf -batch \
		-in tmp/tls1-req.pem -out cfg-aprsc/tls1-cert.pem \
		-days 3650

