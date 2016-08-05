
# Makefile for running the test suite

TEST_FILES = t/*.t
TEST_VERBOSE = 0
INST_LIB = libperl
INST_ARCHLIB = libperl

PERL=/usr/bin/perl

TESTID=`echo $$$$_$$RANDOM$$RANDOM$$RANDOM`
TEST_SRCCALLS="XX7FIT-1,XX7FIT-2"
TEST_IGATE="XX7IG"

test:
	@rm -f logs/aprsc.log
	PERL_DL_NONLAZY=1 $(PERL) "-MExtUtils::Command::MM" "-e" "test_harness($(TEST_VERBOSE), '$(INST_LIB)', '$(INST_ARCHLIB)')" $(TEST_FILES)

quicktest:
	@rm -f logs/aprsc.log
	PERL_DL_NONLAZY=1 $(PERL) "-MExtUtils::Command::MM" "-e" "test_harness($(TEST_VERBOSE), '$(INST_LIB)', '$(INST_ARCHLIB)')" t/01login.t 

AGGREGATOR_FILES = aggregator/*.t

aggrtest:
	@rm -f logs/aprsc.log
	TEST_ID=$(TESTID) TEST_IGATE=$(TEST_IGATE) TEST_SRCCALLS=$(TEST_SRCCALLS) \
	PERL_DL_NONLAZY=1 $(PERL) "-MExtUtils::Command::MM" "-e" "test_harness($(TEST_VERBOSE), '$(INST_LIB)', '$(INST_ARCHLIB)')" $(AGGREGATOR_FILES)

