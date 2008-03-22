
# Makefile for running the test suite

TEST_FILES = t/*.t
TEST_VERBOSE = 0
INST_LIB = .
INST_ARCHLIB = .

PERL=/usr/bin/perl

test:
	PERL_DL_NONLAZY=1 $(PERL) "-MExtUtils::Command::MM" "-e" "test_harness($(TEST_VERBOSE), '$(INST_LIB)', '$(INST_ARCHLIB)')" $(TEST_FILES)

