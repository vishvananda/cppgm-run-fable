.PHONY: run check

RUN_CHECK_DEPS ?= dev-shared-target
RUN_LABEL ?= $(if $(strip $(TARGET)),$(TARGET),$(firstword $(TARGETS)))
RUNNER ?= $(if $(strip $(CPPGM_TEST_APP)),$(CPPGM_TEST_APP),../dev/$(RUN_LABEL))
TARGET_ARGS ?=
CHECK_RUN ?= scripts/run_all_tests.pl $(RUNNER) check "$(TEST)"
CHECK_COMPARE ?= scripts/compare_results.pl ref check "$(TEST)"
REF_TEST_ROOT ?= tests
REF_TEST_INPUT = $(if $(strip $(TEST)),$(TEST),$(REF_TEST_ROOT))
REF_TEST_RUN ?= scripts/run_all_tests.pl $(REF_TEST_APP) ref "$(REF_TEST_INPUT)"

run: $(RUN_CHECK_DEPS)
	@if [ -z "$(INPUT)" ]; then \
		echo "Usage: make run INPUT=<input-file-or-glob> [RUN_OUTPUT=/tmp/$(RUN_LABEL).out]"; \
		exit 1; \
	fi
	@outfile=$${RUN_OUTPUT:-/tmp/$(RUN_LABEL).out}; \
	rm -f "$$outfile"; \
	$(RUNNER) $(TARGET_ARGS) -o "$$outfile" $(INPUT); \
	cat "$$outfile"

check: $(RUN_CHECK_DEPS)
	@if [ -z "$(TEST)" ]; then \
		echo "Usage: make check TEST=<checked-in-test-file>"; \
		exit 1; \
	fi
	@CPPGM_CHECK_MODE=1 $(CHECK_RUN)
	@CPPGM_CHECK_MODE=1 CPPGM_CHECK_AUTO_KEEP_GOING=1 $(CHECK_COMPARE)
