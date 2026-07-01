# cppgm starter makefile

DEFAULT_BUILD_JOBS = $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
HOST_UNAME_S = $(shell uname -s)
ifeq ($(findstring -j,$(MAKEFLAGS)),)
MAKEFLAGS += -j$(DEFAULT_BUILD_JOBS)
endif
ifeq ($(MAKELEVEL),0)
CPPGM_MAKE_JOB_FLAGS := $(filter -j% --jobs=%,$(MAKEFLAGS))
CPPGM_MAKE_JOB_LIMIT := $(patsubst --jobs=%,%,$(patsubst -j%,%,$(lastword $(CPPGM_MAKE_JOB_FLAGS))))
ifneq ($(CPPGM_MAKE_JOB_LIMIT),)
CPPGM_MAKE_LOW_JOB_LIMIT = $(shell \
	limit='$(CPPGM_MAKE_JOB_LIMIT)'; cpus='$(DEFAULT_BUILD_JOBS)'; \
	if [ "$$limit" -gt 0 ] 2>/dev/null && [ "$$cpus" -gt "$$limit" ] 2>/dev/null; then echo 1; else echo 0; fi)
ifeq ($(CPPGM_MAKE_LOW_JOB_LIMIT),1)
$(warning make is limited to -j$(CPPGM_MAKE_JOB_LIMIT) on a $(DEFAULT_BUILD_JOBS)-core machine; large compiler builds, especially self-host and PA39/inception builds, will be very slow. Omit -j or use -j$(DEFAULT_BUILD_JOBS).)
endif
endif
endif
MAKEFLAGS += --no-print-directory


ifeq ($(origin CXX), default)
CXX := g++
endif
export CXX
export CPPGM_STDLIB_FLAGS ?=
ifeq ($(origin CPPGM_HOST_CXX), undefined)
CPPGM_HOST_CXX := $(CXX)
endif
export CPPGM_HOST_CXX
export CPPGM_TEST_RUNNER ?= 1
export CPPGM_TEXT_TEST_TIMEOUT_SEC ?= 10
export CPPGM_BUILD_TEST_TIMEOUT_SEC ?= 30
export CPPGM_PROGRAM_TEST_TIMEOUT_SEC ?= 10
DEBUGINFO_TEST_PAS ?= pa13 pa37 pa38

ALL_PAS = $(patsubst %/Makefile,%,$(wildcard pa*/Makefile))
EXPERIMENTAL_PAS ?= pa39
PAS = $(filter-out $(EXPERIMENTAL_PAS),$(ALL_PAS))
SORTED_PAS = $(shell printf '%s\n' $(PAS) | sort -t a -k 2,2n)
TEST_REPORT_PAS ?= $(SORTED_PAS)
ACTIVE_TEST_REPORT_PAS ?= $(TEST_REPORT_PAS)
REF_TEST_PAS ?= $(SORTED_PAS)
STRICT_PAS ?= pa18 pa19 pa21 pa22 pa23
STRICT_SUBTEST_JOBS ?= $(DEFAULT_BUILD_JOBS)
DEV_BUILD_LOCK = obj/.dev-build.lock

TEST_REPORT_SUBTEST_JOBS ?= 2
TEST_REPORT_ASSIGNMENT_JOBS ?= $(shell subjobs=$(TEST_REPORT_SUBTEST_JOBS); if [ -z "$$subjobs" ] || [ "$$subjobs" -lt 1 ] 2>/dev/null; then subjobs=1; fi; jobs=$$(( $(DEFAULT_BUILD_JOBS) / $$subjobs )); if [ "$$jobs" -lt 1 ]; then jobs=1; fi; echo $$jobs)
TEST_REPORT_STALL_SEC ?= 90
TEST_REPORT_BUILD_TIMEOUT_SEC ?= 60
ORDERED ?= true
SUBMAKE_OBJ_ARG = $(if $(strip $(OBJ)),OBJ=$(OBJ))
SUBMAKE_GENERATED_ARG = $(if $(strip $(GENERATED)),GENERATED=$(GENERATED))
SUBMAKE_CC_FLAGS_ARG = $(if $(strip $(CC_FLAGS)),CC_FLAGS="$(CC_FLAGS)")

.PHONY: all build test ref-test ref-test-strict ref-test-debuginfo \
	test-strict test-strict-nobuild test-debuginfo test-debuginfo-nobuild \
	test-report inception clean run-cppgm run-cppgm-nobuild \
	test-report-nobuild test-report-through-% test-report-through-%-nobuild \
	ref-test-% \
	test-% \
	$(ALL_PAS)
.NOTPARALLEL: ref-test ref-test-strict ref-test-debuginfo

all: build

build:
	@mkdir -p obj
	@lockdir=$(DEV_BUILD_LOCK); \
	while ! mkdir $$lockdir 2>/dev/null; do sleep 1; done; \
	trap 'rmdir "$$lockdir" 2>/dev/null || true' EXIT HUP INT TERM; \
	$(MAKE) -s -C dev all

test: build
	@for dir in $(SORTED_PAS); do \
		echo "===== $$dir ====="; \
		$(MAKE) -C $$dir \
			CXX=$(CXX) \
			CPPGM_HOST_CXX=$(CPPGM_HOST_CXX) \
			CPPGM_STDLIB_FLAGS=$(CPPGM_STDLIB_FLAGS) \
			CPPGM_TEST_RUNNER=$(CPPGM_TEST_RUNNER) \
			$(SUBMAKE_OBJ_ARG) \
			$(SUBMAKE_GENERATED_ARG) \
			$(SUBMAKE_CC_FLAGS_ARG) \
			CPPGM_SKIP_DEV_REBUILD=1 test || exit 1; \
	done
	@echo "===== ALL TESTS PASSED SUCCESSFULLY! ====="

ref-test:
	@for dir in $(REF_TEST_PAS); do \
		echo "===== $$dir (ref-test) ====="; \
		$(MAKE) -C $$dir \
			CXX=$(CXX) \
			CPPGM_HOST_CXX=$(CPPGM_HOST_CXX) \
			CPPGM_STDLIB_FLAGS=$(CPPGM_STDLIB_FLAGS) \
			CPPGM_TEST_RUNNER=$(CPPGM_TEST_RUNNER) \
			$(SUBMAKE_OBJ_ARG) \
			$(SUBMAKE_GENERATED_ARG) \
			$(SUBMAKE_CC_FLAGS_ARG) \
			ref-test || exit 1; \
	done
	@echo "===== ALL REFS REGENERATED SUCCESSFULLY! ====="

ref-test-strict:
	@if [ -z "$(strip $(STRICT_PAS))" ]; then \
		echo "No strict assignments configured"; \
		exit 0; \
	fi
	@for dir in $(STRICT_PAS); do \
		echo "===== $$dir (ref-test-strict) ====="; \
		$(MAKE) -C $$dir \
			CXX=$(CXX) \
			CPPGM_HOST_CXX=$(CPPGM_HOST_CXX) \
			CPPGM_STDLIB_FLAGS=$(CPPGM_STDLIB_FLAGS) \
			CPPGM_TEST_RUNNER=$(CPPGM_TEST_RUNNER) \
			$(SUBMAKE_OBJ_ARG) \
			$(SUBMAKE_GENERATED_ARG) \
			$(SUBMAKE_CC_FLAGS_ARG) \
			ref-test-strict || exit 1; \
	done
	@echo "===== ALL STRICT WITNESS REFS REGENERATED SUCCESSFULLY! ====="

ref-test-debuginfo:
	@if [ -z "$(strip $(DEBUGINFO_TEST_PAS))" ]; then \
		echo "No debuginfo assignments configured"; \
		exit 0; \
	fi
	@for dir in $(DEBUGINFO_TEST_PAS); do \
		echo "===== $$dir (ref-test-debuginfo) ====="; \
		$(MAKE) -C $$dir \
			CXX=$(CXX) \
			CPPGM_HOST_CXX=$(CPPGM_HOST_CXX) \
			CPPGM_STDLIB_FLAGS=$(CPPGM_STDLIB_FLAGS) \
			CPPGM_TEST_RUNNER=$(CPPGM_TEST_RUNNER) \
			$(SUBMAKE_OBJ_ARG) \
			$(SUBMAKE_GENERATED_ARG) \
			$(SUBMAKE_CC_FLAGS_ARG) \
			ref-test-debuginfo || exit 1; \
	done
	@echo "===== ALL DEBUGINFO REFS REGENERATED SUCCESSFULLY! ====="

test-strict: build
	@$(MAKE) test-strict-nobuild \
		STRICT_PAS='$(STRICT_PAS)'

test-strict-nobuild:
	@export KEEP_GOING=1; \
	if [ "$(CPPGM_TEST_RUNNER)" = "1" ]; then \
		export CPPGM_BATCH_TESTS=1; \
		export WRAPPED_BATCH_STDIN=1; \
	else \
		unset CPPGM_BATCH_TESTS; \
		unset WRAPPED_BATCH_STDIN; \
	fi; \
	export CPPGM_TEST_JOBS=$(STRICT_SUBTEST_JOBS); \
	if [ -z "$(strip $(STRICT_PAS))" ]; then \
		echo "===== NO STRICT TESTS CONFIGURED ====="; \
		exit 0; \
	fi; \
	status=0; \
	failed=''; \
	for dir in $(STRICT_PAS); do \
		echo "===== $$dir (strict) ====="; \
		$(MAKE) -C $$dir \
			CXX=$(CXX) \
			CPPGM_HOST_CXX=$(CPPGM_HOST_CXX) \
			CPPGM_STDLIB_FLAGS=$(CPPGM_STDLIB_FLAGS) \
			CPPGM_TEST_RUNNER=$(CPPGM_TEST_RUNNER) \
			$(SUBMAKE_OBJ_ARG) \
			$(SUBMAKE_GENERATED_ARG) \
			$(SUBMAKE_CC_FLAGS_ARG) \
			CPPGM_SKIP_DEV_REBUILD=1 test-strict || { status=1; failed="$$failed $$dir"; }; \
	done; \
	if [ $$status -ne 0 ]; then \
		echo "===== STRICT TESTS FAILED IN:$${failed} ====="; \
		exit $$status; \
	fi; \
	echo "===== ALL STRICT TESTS PASSED SUCCESSFULLY! ====="

test-debuginfo: build
	@$(MAKE) test-debuginfo-nobuild \
		DEBUGINFO_TEST_PAS='$(DEBUGINFO_TEST_PAS)'

test-debuginfo-nobuild:
	@if [ -z "$(strip $(DEBUGINFO_TEST_PAS))" ]; then \
		echo "===== NO DEBUG-INFO TESTS CONFIGURED ====="; \
		exit 0; \
	fi
	@status=0; failed=""; \
	for dir in $(DEBUGINFO_TEST_PAS); do \
		echo "===== $$dir (debug info) ====="; \
		$(MAKE) -C $$dir \
			CXX=$(CXX) \
			CPPGM_HOST_CXX=$(CPPGM_HOST_CXX) \
			CPPGM_STDLIB_FLAGS=$(CPPGM_STDLIB_FLAGS) \
			CPPGM_TEST_RUNNER=$(CPPGM_TEST_RUNNER) \
			$(SUBMAKE_OBJ_ARG) \
			$(SUBMAKE_GENERATED_ARG) \
			$(SUBMAKE_CC_FLAGS_ARG) \
			CPPGM_SKIP_DEV_REBUILD=1 test-debuginfo || { status=1; failed="$$failed $$dir"; }; \
	done; \
	if [ $$status -ne 0 ]; then \
		echo "===== DEBUG-INFO TESTS FAILED IN:$${failed} ====="; \
		exit $$status; \
	fi; \
	echo "===== DEBUG-INFO TESTS PASSED SUCCESSFULLY! ====="

inception: build
	@$(MAKE) -C pa39 \
		CXX=../dev/cppgm++ \
		CPPGM_HOST_CXX="$(CPPGM_HOST_CXX)" \
		CPPGM_STDLIB_FLAGS="$(CPPGM_STDLIB_FLAGS)" \
		compare-cppgm++-inception

test-report: build
	@$(MAKE) test-report-nobuild \
		ACTIVE_TEST_REPORT_PAS='$(ACTIVE_TEST_REPORT_PAS)' \
		TEST_REPORT_ASSIGNMENT_JOBS='$(TEST_REPORT_ASSIGNMENT_JOBS)' \
		TEST_REPORT_SUBTEST_JOBS='$(TEST_REPORT_SUBTEST_JOBS)' \
		ORDERED='$(ORDERED)'

test-report-through-%-nobuild:
	@target='$*'; \
	max=$${target#pa}; \
	pas=''; \
	for dir in $(SORTED_PAS); do \
		num=$${dir#pa}; \
		if [ "$$num" -le "$$max" ]; then \
			pas="$$pas $$dir"; \
		fi; \
	done; \
	$(MAKE) test-report-nobuild \
		ACTIVE_TEST_REPORT_PAS="$$pas" \
		TEST_REPORT_ASSIGNMENT_JOBS='$(TEST_REPORT_ASSIGNMENT_JOBS)' \
		TEST_REPORT_SUBTEST_JOBS='$(TEST_REPORT_SUBTEST_JOBS)' \
		ORDERED='$(ORDERED)'

test-report-through-%: build
	@target='$*'; \
	max=$${target#pa}; \
	pas=''; \
	for dir in $(SORTED_PAS); do \
		num=$${dir#pa}; \
		if [ "$$num" -le "$$max" ]; then \
			pas="$$pas $$dir"; \
		fi; \
	done; \
	$(MAKE) test-report-nobuild \
		ACTIVE_TEST_REPORT_PAS="$$pas" \
		TEST_REPORT_ASSIGNMENT_JOBS='$(TEST_REPORT_ASSIGNMENT_JOBS)' \
		TEST_REPORT_SUBTEST_JOBS='$(TEST_REPORT_SUBTEST_JOBS)' \
		ORDERED='$(ORDERED)'

test-report-nobuild:
	@export KEEP_GOING=1; \
	if [ "$(CPPGM_TEST_RUNNER)" = "1" ]; then \
		export CPPGM_BATCH_TESTS=1; \
		export WRAPPED_BATCH_STDIN=1; \
	else \
		unset CPPGM_BATCH_TESTS; \
		unset WRAPPED_BATCH_STDIN; \
	fi; \
	export CPPGM_TEST_JOBS=$(TEST_REPORT_SUBTEST_JOBS); \
	export CPPGM_BUILD_TEST_TIMEOUT_SEC=$(TEST_REPORT_BUILD_TIMEOUT_SEC); \
	stall_sec=$(TEST_REPORT_STALL_SEC); \
	ordered="$(ORDERED)"; \
	tmpdir=$$(mktemp -d); \
	cleanup() { rm -f pa*/.test_failed .test_counts; rm -rf "$$tmpdir"; }; \
	interrupt() { \
		if [ -n "$$output_pid" ]; then \
			kill -TERM "$$output_pid" 2>/dev/null || true; \
			wait "$$output_pid" 2>/dev/null || true; \
		fi; \
		if [ -n "$$monitor_pid" ]; then \
			kill -TERM "$$monitor_pid" 2>/dev/null || true; \
			wait "$$monitor_pid" 2>/dev/null || true; \
		fi; \
		if [ -n "$$xargs_pid" ]; then \
			pkill -TERM -P "$$xargs_pid" 2>/dev/null || true; \
			kill -TERM "$$xargs_pid" 2>/dev/null || true; \
			wait "$$xargs_pid" 2>/dev/null || true; \
		fi; \
		cleanup; \
		exit 130; \
	}; \
	trap 'cleanup' EXIT; \
	trap 'interrupt' INT TERM; \
	rm -f pa*/.test_failed .test_counts; \
	printf '%s\n' $(ACTIVE_TEST_REPORT_PAS) > "$$tmpdir/pas.list"; \
	xargs -P$(TEST_REPORT_ASSIGNMENT_JOBS) -I{} /bin/bash -lc '\
		dir="$$1"; \
		echo "===== $$dir =====" > "$$2/$$dir.out"; \
		status=0; \
		env MAKEFLAGS= $(MAKE) --no-print-directory -j1 -C "$$dir" \
			CXX="$$CXX" \
			CPPGM_HOST_CXX="$$CPPGM_HOST_CXX" \
			CPPGM_PROGRESS_FILE="$$2/$$dir.progress" \
			CPPGM_STDLIB_FLAGS="$$CPPGM_STDLIB_FLAGS" \
			CPPGM_TEST_RUNNER="$$CPPGM_TEST_RUNNER" \
			CPPGM_TEST_JOBS="$$CPPGM_TEST_JOBS" \
			$(if $(strip $(OBJ)),OBJ="$(OBJ)") \
			$(if $(strip $(GENERATED)),GENERATED="$(GENERATED)") \
			$(if $(strip $(CC_FLAGS)),CC_FLAGS="$(CC_FLAGS)") \
			CPPGM_SKIP_DEV_REBUILD=1 \
			test >> "$$2/$$dir.out" 2>&1 || status=$$?; \
		printf "%s\n" "$$status" > "$$2/$$dir.status"; \
		: > "$$2/$$dir.done"; \
		exit "$$status"' _ {} "$$tmpdir" < "$$tmpdir/pas.list" & \
	xargs_pid=$$!; \
	emit_completed_output() { \
		dir="$$1"; \
		if [ -f "$$tmpdir/$$dir.out" ] && [ ! -f "$$tmpdir/$$dir.printed" ]; then \
			cat "$$tmpdir/$$dir.out"; \
			: > "$$tmpdir/$$dir.printed"; \
		fi; \
	}; \
	stream_completed_outputs() { \
		while kill -0 "$$xargs_pid" 2>/dev/null; do \
			sleep 1; \
			for dir in $(ACTIVE_TEST_REPORT_PAS); do \
				if [ -f "$$tmpdir/$$dir.done" ]; then \
					emit_completed_output "$$dir"; \
				fi; \
			done; \
		done; \
		for dir in $(ACTIVE_TEST_REPORT_PAS); do \
			emit_completed_output "$$dir"; \
		done; \
	}; \
	monitor_progress() { \
		while kill -0 "$$xargs_pid" 2>/dev/null; do \
			sleep 5; \
			for dir in $(ACTIVE_TEST_REPORT_PAS); do \
				progress_file="$$tmpdir/$$dir.progress"; \
				stall_file="$$tmpdir/$$dir.stalled"; \
				if [ ! -f "$$progress_file" ]; then \
					rm -f "$$stall_file"; \
					continue; \
				fi; \
				updated=$$(awk -F '\t' 'NR==1 { print $$1 }' "$$progress_file" 2>/dev/null); \
				phase=$$(awk -F '\t' 'NR==1 { print $$3 }' "$$progress_file" 2>/dev/null); \
				test_name=$$(awk -F '\t' 'NR==1 { print $$4 }' "$$progress_file" 2>/dev/null); \
				if [ -z "$$updated" ]; then \
					continue; \
				fi; \
				now=$$(date +%s); \
				idle=$$((now - updated)); \
				if [ "$$idle" -lt "$$stall_sec" ]; then \
					rm -f "$$stall_file"; \
					continue; \
				fi; \
				signature="$$updated	$$phase	$$test_name"; \
				last_signature=""; \
				if [ -f "$$stall_file" ]; then \
					last_signature=$$(cat "$$stall_file"); \
				fi; \
				if [ "$$signature" != "$$last_signature" ]; then \
					echo "===== $$dir waiting $$idle s at $$phase $$test_name ====="; \
					printf '%s' "$$signature" > "$$stall_file"; \
				fi; \
			done; \
		done; \
	}; \
	if [ "$$stall_sec" -gt 0 ] 2>/dev/null; then \
		monitor_progress & \
		monitor_pid=$$!; \
	fi; \
	if [ "$$ordered" = "false" ]; then \
		stream_completed_outputs & \
		output_pid=$$!; \
	fi; \
	wait "$$xargs_pid"; \
	xargs_status=$$?; \
	if [ -n "$$output_pid" ]; then \
		kill -TERM "$$output_pid" 2>/dev/null || true; \
		wait "$$output_pid" 2>/dev/null || true; \
	fi; \
	if [ -n "$$monitor_pid" ]; then \
		kill -TERM "$$monitor_pid" 2>/dev/null || true; \
		wait "$$monitor_pid" 2>/dev/null || true; \
	fi; \
	for dir in $(ACTIVE_TEST_REPORT_PAS); do \
		if [ "$$ordered" = "false" ]; then \
			emit_completed_output "$$dir"; \
		elif [ -f "$$tmpdir/$$dir.out" ]; then \
			cat "$$tmpdir/$$dir.out"; \
		fi; \
	done; \
	passed=$$(awk '{s+=$$1} END {print s}' .test_counts 2>/dev/null || echo 0); \
	total=$$(awk '{s+=$$2} END {print s}' .test_counts 2>/dev/null || echo 0); \
	if ls pa*/.test_failed 1>/dev/null 2>&1; then \
		echo "===== TEST SUMMARY: $$passed / $$total TESTS PASSED ====="; \
		exit 1; \
	elif [ "$$xargs_status" -ne 0 ]; then \
		echo "===== TEST SUMMARY: $$passed / $$total TESTS PASSED ====="; \
		exit "$$xargs_status"; \
	else \
		echo "===== ALL TESTS PASSED SUCCESSFULLY! ($$passed / $$total) ====="; \
	fi

run-cppgm:
	@if [ -z "$(strip $(CPPGM_ARGS))" ]; then \
		echo "usage: make run-cppgm CPPGM_ARGS='...'" >&2; \
		exit 2; \
	fi
	@$(MAKE) build
	@$(MAKE) run-cppgm-nobuild CPPGM_ARGS='$(CPPGM_ARGS)'

run-cppgm-nobuild:
	@if [ -z "$(strip $(CPPGM_ARGS))" ]; then \
		echo "usage: make run-cppgm-nobuild CPPGM_ARGS='...'" >&2; \
		exit 2; \
	fi
	@./dev/cppgm++ $(CPPGM_ARGS)

clean:
	-$(MAKE) -C dev clean
	@for dir in $(sort $(ALL_PAS)); do \
		$(MAKE) -C $$dir clean || exit 1; \
	done
	-rm -f pa*/.test_failed .test_counts
	-rmdir $(DEV_BUILD_LOCK) 2>/dev/null || true

$(ALL_PAS):
	$(MAKE) build
	$(MAKE) -C $@ \
		CXX=$(CXX) \
		CPPGM_HOST_CXX=$(CPPGM_HOST_CXX) \
		CPPGM_STDLIB_FLAGS=$(CPPGM_STDLIB_FLAGS) \
		CPPGM_TEST_RUNNER=$(CPPGM_TEST_RUNNER) \
		$(SUBMAKE_OBJ_ARG) \
		$(SUBMAKE_GENERATED_ARG) \
		$(SUBMAKE_CC_FLAGS_ARG) \
		CPPGM_SKIP_DEV_REBUILD=1 test

test-strict-%:
	@if [ ! -f "$*/Makefile" ]; then \
		echo "unknown assignment: $*" >&2; \
		exit 2; \
	fi
	@$(MAKE) build
	@if [ "$(CPPGM_TEST_RUNNER)" = "1" ]; then \
		export CPPGM_BATCH_TESTS=1; \
		export WRAPPED_BATCH_STDIN=1; \
	else \
		unset CPPGM_BATCH_TESTS; \
		unset WRAPPED_BATCH_STDIN; \
	fi; \
	export CPPGM_TEST_JOBS=$(STRICT_SUBTEST_JOBS); \
	$(MAKE) -C $* \
		CXX=$(CXX) \
		CPPGM_HOST_CXX=$(CPPGM_HOST_CXX) \
		CPPGM_STDLIB_FLAGS=$(CPPGM_STDLIB_FLAGS) \
		CPPGM_TEST_RUNNER=$(CPPGM_TEST_RUNNER) \
		$(SUBMAKE_OBJ_ARG) \
		$(SUBMAKE_GENERATED_ARG) \
		$(SUBMAKE_CC_FLAGS_ARG) \
		CPPGM_SKIP_DEV_REBUILD=1 test-strict

ref-test-%:
	@if [ ! -f "$*/Makefile" ]; then \
		echo "unknown assignment: $*" >&2; \
		exit 2; \
	fi
	@$(MAKE) -C $* \
		CXX=$(CXX) \
		CPPGM_HOST_CXX=$(CPPGM_HOST_CXX) \
		CPPGM_STDLIB_FLAGS=$(CPPGM_STDLIB_FLAGS) \
		CPPGM_TEST_RUNNER=$(CPPGM_TEST_RUNNER) \
		$(SUBMAKE_OBJ_ARG) \
		$(SUBMAKE_GENERATED_ARG) \
		$(SUBMAKE_CC_FLAGS_ARG) \
		$(if $(strip $(TEST)),TEST=$(patsubst $*/%,%,$(TEST)),) \
		ref-test

test-%:
	@if [ ! -f "$*/Makefile" ]; then \
		echo "unknown assignment: $*" >&2; \
		exit 2; \
	fi
	@$(MAKE) build
	@$(MAKE) -C $* \
		CXX=$(CXX) \
		CPPGM_HOST_CXX=$(CPPGM_HOST_CXX) \
		CPPGM_STDLIB_FLAGS=$(CPPGM_STDLIB_FLAGS) \
		CPPGM_TEST_RUNNER=$(CPPGM_TEST_RUNNER) \
		$(SUBMAKE_OBJ_ARG) \
		$(SUBMAKE_GENERATED_ARG) \
		$(SUBMAKE_CC_FLAGS_ARG) \
		CPPGM_SKIP_DEV_REBUILD=1 test

reference-binaries:
	@scripts/ensure_reference_binaries.pl

setup: reference-binaries

.PHONY: reference-binaries setup
