TARGET = cppgm++
TARGET_ARGS =
export CPPGM_APP_ARGS := $(TARGET_ARGS)
HOST_UNAME_S = $(shell uname -s)

ifeq ($(origin CXX), default)
CXX := g++
endif
ifeq ($(origin CPPGM_HOST_CXX), undefined)
CPPGM_HOST_CXX := $(CXX)
endif
export CPPGM_HOST_CXX
CPPGM_STDLIB_FLAGS ?=
CPPGM_TEST_RUNNER ?= 1
REPORT_EMPTY_OPTIONAL_TEST_ROOTS ?= 0
CPPGM_TEST_APP ?= ../dev/$(TARGET)
REF_TEST_APP ?= $(CPPGM_TEST_APP)-ref
TEST_DEPS ?= dev-shared-target
RUN_CHECK_DEPS ?= dev-shared-target
LINK_TEST_ROOT ?= tests/link
COURSE_LINK_TEST_ROOT ?= course/pa36/link
TEST_RUNNER_SHARED_FLAGS = $(if $(filter 1,$(CPPGM_TEST_RUNNER)),-DTEST_RUNNER_ENABLE,)
TEST_RUNNER_ENTRY_FLAGS = $(if $(filter 1,$(CPPGM_TEST_RUNNER)),-Dmain=test_runner_real_main,)
HOST_CXX_DEFAULT_DEF = -DCPPGM_DEFAULT_HOST_CXX='"$(CPPGM_HOST_CXX)"'
OBJECT_ROOT_DEFAULT_DEF = -DCPPGM_DEFAULT_OBJECT_ROOT='"$(abspath $(OBJ))"'
CC_FLAGS = -std=gnu++11 -Wall -O3 $(CPPGM_STDLIB_FLAGS) $(HOST_CXX_DEFAULT_DEF) $(OBJECT_ROOT_DEFAULT_DEF)
DEBUG_CC_FLAGS = -std=gnu++11 -Wall -O0 -g -fno-omit-frame-pointer $(CPPGM_STDLIB_FLAGS) $(HOST_CXX_DEFAULT_DEF) $(OBJECT_ROOT_DEFAULT_DEF)
ENTRY_CC_FLAGS = $(CC_FLAGS) $(TEST_RUNNER_ENTRY_FLAGS)
ENTRY_DEBUG_CC_FLAGS = $(DEBUG_CC_FLAGS) $(TEST_RUNNER_ENTRY_FLAGS)
SRC = ../dev/src
OBJ ?= ../obj
export CPPGM_OBJECT_ROOT := $(abspath $(OBJ))
GENERATED ?= $(OBJ)/generated
BUILTIN_HOST_CONFIG = $(GENERATED)/cppgm_builtin_host_config.h
INC = -I$(SRC) -I$(GENERATED)
SHARED_OBJ_ROOT = $(abspath ../obj)
CURRENT_OBJ_ROOT = $(abspath $(OBJ))
ifeq ($(CURRENT_OBJ_ROOT),$(SHARED_OBJ_ROOT))
ifneq ($(abspath $(firstword $(CXX))),$(abspath $(firstword $(CPPGM_HOST_CXX))))
$(error shared OBJ root $(OBJ) is only supported with host compiler $(CPPGM_HOST_CXX); use OBJ=<isolated-root> for selfhost or other non-host builds)
endif
endif
VPATH = $(SRC)
RDEPDIR = $(OBJ)/release/.d
DDEPDIR = $(OBJ)/debug/.d
$(shell mkdir -p $(RDEPDIR) $(DDEPDIR) >/dev/null)

include ../dev/frontend_source_sets.mk

$(BUILTIN_HOST_CONFIG): FORCE
	@mkdir -p $(dir $@)
	@CPPGM_BUILD_HOST_CXX='$(CPPGM_HOST_CXX)' \
		CPPGM_STDLIB_FLAGS='$(CPPGM_STDLIB_FLAGS)' \
		perl ../dev/gen_builtin_host_config.pl > $@.tmp
	@if ! cmp -s $@.tmp $@ 2>/dev/null; then mv $@.tmp $@; else rm -f $@.tmp; fi

$(OBJ)/release/preprocessor.o $(OBJ)/debug/preprocessor.o: $(BUILTIN_HOST_CONFIG)

common_obj_basenames = $(FRONTEND_OBJ_BASENAMES_$(TARGET))
common_objs = $(addprefix $(OBJ)/$(1)/,$(addsuffix .o,$(common_obj_basenames)))
runner_obj = $(if $(filter 1,$(CPPGM_TEST_RUNNER)),$(OBJ)/$(1)/test_runner_enabled.o)
runner_state_stamp = $(OBJ)/$(1)/.test_runner_mode
link_objs = $(call common_objs,$(1)) $(call runner_obj,$(1))
link_prereqs = $(call link_objs,$(1)) $(call runner_state_stamp,$(1))

all: dev-shared-target

$(OBJ)/release $(OBJ)/debug: %:
	@- mkdir -p $@

FORCE:

$(OBJ)/release/.test_runner_mode $(OBJ)/debug/.test_runner_mode: FORCE | $(OBJ)/release $(OBJ)/debug
	@current=$$(cat $@ 2>/dev/null || true); \
	if [ "$$current" != "$(CPPGM_TEST_RUNNER)" ]; then \
		printf '%s\n' '$(CPPGM_TEST_RUNNER)' > $@; \
	fi

$(OBJ)/release/%.o: %.cpp
	@mkdir -p $(@D) $(dir $(RDEPDIR)/$*)
	$(CXX) $(CC_FLAGS) $(INC) -c -MT $@ -MMD -MP -MF $(RDEPDIR)/$*.Td -o $@ $<
	mv -f $(RDEPDIR)/$*.Td $(RDEPDIR)/$*.d

$(OBJ)/debug/%.o: %.cpp
	@mkdir -p $(@D) $(dir $(DDEPDIR)/$*)
	$(CXX) $(DEBUG_CC_FLAGS) $(INC) -c -MT $@ -MMD -MP -MF $(DDEPDIR)/$*.Td -o $@ $<
	mv -f $(DDEPDIR)/$*.Td $(DDEPDIR)/$*.d

$(OBJ)/release/test_runner_enabled.o: $(SRC)/test_runner.cpp
	$(CXX) $(CC_FLAGS) $(TEST_RUNNER_SHARED_FLAGS) $(INC) -c -MT $@ -MMD -MP -MF $(RDEPDIR)/test_runner_enabled.Td -o $@ $<
	mv -f $(RDEPDIR)/test_runner_enabled.Td $(RDEPDIR)/test_runner_enabled.d

$(OBJ)/debug/test_runner_enabled.o: $(SRC)/test_runner.cpp
	$(CXX) $(DEBUG_CC_FLAGS) $(TEST_RUNNER_SHARED_FLAGS) $(INC) -c -MT $@ -MMD -MP -MF $(DDEPDIR)/test_runner_enabled.Td -o $@ $<
	mv -f $(DDEPDIR)/test_runner_enabled.Td $(DDEPDIR)/test_runner_enabled.d

define run_link_course_bucket
	@if [ -d "$(1)" ] && find "$(1)" -type f -name '*.t' | grep -q .; then \
		perl ../scripts/run_cpphostinterop_tests_worker.pl $(CPPGM_TEST_APP) my "$(1)"; \
		scripts/compare_results.pl ref my "$(1)"; \
	else \
		if [ "$(REPORT_EMPTY_OPTIONAL_TEST_ROOTS)" = "1" ]; then echo "$(TARGET) $(1): no tests"; fi; \
	fi
endef

test: $(TEST_DEPS)
	@perl ../scripts/run_cpphostinterop_tests_worker.pl $(CPPGM_TEST_APP) my $(LINK_TEST_ROOT)
	@scripts/compare_results.pl ref my $(LINK_TEST_ROOT)
	$(call run_link_course_bucket,$(COURSE_LINK_TEST_ROOT))

test-batch: $(TEST_DEPS)
	@perl ../scripts/run_cpphostinterop_tests_worker.pl $(CPPGM_TEST_APP) my $(LINK_TEST_ROOT)
	@scripts/compare_results.pl ref my $(LINK_TEST_ROOT)
	$(call run_link_course_bucket,$(COURSE_LINK_TEST_ROOT))

ref-test:
ifneq ($(strip $(TEST)),)
	@$(REF_TEST_RUN)
else
	@CPPGM_CMAKE_COMPILER=$(REF_TEST_APP) \
		perl ../scripts/run_cpphostinterop_tests_worker.pl $(REF_TEST_APP) ref $(LINK_TEST_ROOT)
endif

ref-test-batch:
	@CPPGM_CMAKE_COMPILER=$(REF_TEST_APP) \
		perl ../scripts/run_cpphostinterop_tests_worker.pl $(REF_TEST_APP) ref $(LINK_TEST_ROOT)

REF_TEST_RUN = case "$(TEST)" in \
	$(LINK_TEST_ROOT)/*|$(COURSE_LINK_TEST_ROOT)/*) CPPGM_CMAKE_COMPILER=$(REF_TEST_APP) perl ../scripts/run_cpphostinterop_tests_worker.pl $(REF_TEST_APP) ref "$(TEST)" ;; \
	*) echo "Usage: make ref-test TEST=$(LINK_TEST_ROOT)/<file>.t"; exit 1 ;; \
esac

CHECK_RUN = perl ../scripts/run_cpphostinterop_tests_worker.pl $(CPPGM_TEST_APP) check "$(TEST)"
include ../scripts/pa_run_check_targets.mk

.PHONY: dev-shared-target ref-test ref-test-batch test-batch
../dev/$(TARGET):
	$(MAKE) -C ../dev $(TARGET) CXX=$(CXX)

dev-shared-target:
	@if [ "$(CPPGM_SKIP_DEV_REBUILD)" = "1" ]; then true; else $(MAKE) -C ../dev $(TARGET) CXX=$(CXX); fi

clean:
	-rm -f "$(TARGET)" "$(TARGET)-debug" $(OBJ)/release/*.o $(OBJ)/debug/*.o

$(RDEPDIR)/%.d: ;
.PRECIOUS: $(RDEPDIR)/%.d

$(DDEPDIR)/%.d: ;
.PRECIOUS: $(DDEPDIR)/%.d

-include $(addprefix $(RDEPDIR)/,$(addsuffix .d,$(common_obj_basenames)))
-include $(addprefix $(DDEPDIR)/,$(addsuffix .d,$(common_obj_basenames)))
-include $(RDEPDIR)/test_runner_enabled.d
-include $(DDEPDIR)/test_runner_enabled.d
