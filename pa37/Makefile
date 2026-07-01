TARGET = lowiropt
TARGET_ARGS =
export CPPGM_APP_ARGS := $(TARGET_ARGS)
LOWIROPT_APP ?= ../dev/$(TARGET)
CPPGM_DRIVER_APP ?= ../dev/cppgm++
LOWIROPT_REF_APP ?= $(LOWIROPT_APP)-ref
CPPGM_DRIVER_REF_APP ?= $(CPPGM_DRIVER_APP)-ref
TEST_DEPS ?= dev-shared-target
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
O0_TEST_ROOT ?= tests/o0
O1_TEST_ROOT ?= tests/o1
O2_TEST_ROOT ?= tests/o2
DRIVER_O1_TEST_ROOT ?= tests/driver/o1
DRIVER_O2_TEST_ROOT ?= tests/driver/o2
OBJECT_ROUNDTRIP_TEST_ROOT ?= tests/object-roundtrip
DEBUGINFO_O2_TEST_ROOT ?= tests/debuginfo/o2
DEBUGINFO_DRIVER_O2_TEST_ROOT ?= tests/debuginfo/driver/o2
DEBUGINFO_O1_TEST_ROOT ?= tests/debuginfo/o1
DEBUGINFO_DRIVER_O1_TEST_ROOT ?= tests/debuginfo/driver/o1
COURSE_O0_TEST_ROOT ?= course/pa37/o0
COURSE_O1_TEST_ROOT ?= course/pa37/o1
COURSE_O2_TEST_ROOT ?= course/pa37/o2
COURSE_DRIVER_O1_TEST_ROOT ?= course/pa37/driver/o1
COURSE_DRIVER_O2_TEST_ROOT ?= course/pa37/driver/o2
COURSE_OBJECT_ROUNDTRIP_TEST_ROOT ?= course/pa37/object-roundtrip
COURSE_DEBUGINFO_O1_TEST_ROOT ?= course/pa37/debuginfo/o1
COURSE_DEBUGINFO_O2_TEST_ROOT ?= course/pa37/debuginfo/o2
COURSE_DEBUGINFO_DRIVER_O1_TEST_ROOT ?= course/pa37/debuginfo/driver/o1
COURSE_DEBUGINFO_DRIVER_O2_TEST_ROOT ?= course/pa37/debuginfo/driver/o2
TEST_RUNNER_SHARED_FLAGS = $(if $(filter 1,$(CPPGM_TEST_RUNNER)),-DTEST_RUNNER_ENABLE,)
TEST_RUNNER_ENTRY_FLAGS = $(TEST_RUNNER_SHARED_FLAGS) $(if $(filter 1,$(CPPGM_TEST_RUNNER)),-Dmain=test_runner_real_main,)
CC_FLAGS = -std=gnu++11 -Wall -O3 $(CPPGM_STDLIB_FLAGS)
ENTRY_CC_FLAGS = $(CC_FLAGS) $(TEST_RUNNER_ENTRY_FLAGS)
SRC = ../dev/src
GENERATED = ../obj/generated
INC = -I$(SRC) -I$(GENERATED)
OBJ = ../obj
VPATH = $(SRC)
RDEPDIR = $(OBJ)/release/.d
DDEPDIR = $(OBJ)/debug/.d
$(shell mkdir -p $(RDEPDIR) $(DDEPDIR) >/dev/null)

include ../dev/frontend_source_sets.mk

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
	$(CXX) $(CC_FLAGS) $(INC) -c -MT $@ -MMD -MP -MF $(DDEPDIR)/$*.Td -o $@ $<
	mv -f $(DDEPDIR)/$*.Td $(DDEPDIR)/$*.d

$(OBJ)/release/test_runner_enabled.o: $(SRC)/test_runner.cpp
	$(CXX) $(CC_FLAGS) $(TEST_RUNNER_SHARED_FLAGS) $(INC) -c -MT $@ -MMD -MP -MF $(RDEPDIR)/test_runner_enabled.Td -o $@ $<
	mv -f $(RDEPDIR)/test_runner_enabled.Td $(RDEPDIR)/test_runner_enabled.d

$(OBJ)/debug/test_runner_enabled.o: $(SRC)/test_runner.cpp
	$(CXX) $(CC_FLAGS) $(TEST_RUNNER_SHARED_FLAGS) $(INC) -c -MT $@ -MMD -MP -MF $(DDEPDIR)/test_runner_enabled.Td -o $@ $<
	mv -f $(DDEPDIR)/test_runner_enabled.Td $(DDEPDIR)/test_runner_enabled.d

define run_text_course_bucket
	@if [ -d "$(3)" ] && find "$(3)" -type f -name '*.t' | grep -q .; then \
		CPPGM_APP_ARGS='$(1)' scripts/run_all_tests.pl $(2) my "$(3)"; \
		scripts/compare_results.pl ref my "$(3)"; \
	else \
		if [ "$(REPORT_EMPTY_OPTIONAL_TEST_ROOTS)" = "1" ]; then echo "$(TARGET) $(3): no tests"; fi; \
	fi
endef

define run_object_roundtrip_bucket
	@if [ -d "$(1)" ] && find "$(1)" \( -type f -o -type l \) \( -name '*.cpp' -o -name '*.t' \) | grep -q .; then \
		scripts/run_object_lowir_roundtrip_tests.pl $(2) --app "$(CPPGM_DRIVER_APP)" --test-root "$(1)"; \
	else \
		if [ "$(REPORT_EMPTY_OPTIONAL_TEST_ROOTS)" = "1" ]; then echo "$(TARGET) $(1): no tests"; fi; \
	fi
endef

test: $(TEST_DEPS)
	@CPPGM_APP_ARGS='-O0' scripts/run_all_tests.pl $(LOWIROPT_APP) my $(O0_TEST_ROOT)
	@scripts/compare_results.pl ref my $(O0_TEST_ROOT)
	$(call run_text_course_bucket,-O0,$(LOWIROPT_APP),$(COURSE_O0_TEST_ROOT))
	@CPPGM_APP_ARGS='-O1' scripts/run_all_tests.pl $(LOWIROPT_APP) my $(O1_TEST_ROOT)
	@scripts/compare_results.pl ref my $(O1_TEST_ROOT)
	$(call run_text_course_bucket,-O1,$(LOWIROPT_APP),$(COURSE_O1_TEST_ROOT))
	@CPPGM_APP_ARGS='-O2' scripts/run_all_tests.pl $(LOWIROPT_APP) my $(O2_TEST_ROOT)
	@scripts/compare_results.pl ref my $(O2_TEST_ROOT)
	$(call run_text_course_bucket,-O2,$(LOWIROPT_APP),$(COURSE_O2_TEST_ROOT))
	@CPPGM_APP_ARGS='--emit-lowir -g0 -O1' scripts/run_all_tests.pl $(CPPGM_DRIVER_APP) my $(DRIVER_O1_TEST_ROOT)
	@scripts/compare_results.pl ref my $(DRIVER_O1_TEST_ROOT)
	$(call run_text_course_bucket,--emit-lowir -g0 -O1,$(CPPGM_DRIVER_APP),$(COURSE_DRIVER_O1_TEST_ROOT))
	@CPPGM_APP_ARGS='--emit-lowir -g0 -O2' scripts/run_all_tests.pl $(CPPGM_DRIVER_APP) my $(DRIVER_O2_TEST_ROOT)
	@scripts/compare_results.pl ref my $(DRIVER_O2_TEST_ROOT)
	$(call run_text_course_bucket,--emit-lowir -g0 -O2,$(CPPGM_DRIVER_APP),$(COURSE_DRIVER_O2_TEST_ROOT))
	$(call run_object_roundtrip_bucket,$(OBJECT_ROUNDTRIP_TEST_ROOT))
	$(call run_object_roundtrip_bucket,$(COURSE_OBJECT_ROUNDTRIP_TEST_ROOT))

test-debuginfo: $(TEST_DEPS)
	@CPPGM_APP_ARGS='-O1' scripts/run_all_tests.pl $(LOWIROPT_APP) my $(DEBUGINFO_O1_TEST_ROOT)
	@scripts/compare_results.pl ref my $(DEBUGINFO_O1_TEST_ROOT)
	$(call run_text_course_bucket,-O1,$(LOWIROPT_APP),$(COURSE_DEBUGINFO_O1_TEST_ROOT))
	@CPPGM_APP_ARGS='-O2' scripts/run_all_tests.pl $(LOWIROPT_APP) my $(DEBUGINFO_O2_TEST_ROOT)
	@scripts/compare_results.pl ref my $(DEBUGINFO_O2_TEST_ROOT)
	$(call run_text_course_bucket,-O2,$(LOWIROPT_APP),$(COURSE_DEBUGINFO_O2_TEST_ROOT))
	@CPPGM_APP_ARGS='--emit-lowir -gline-tables-only -O1' scripts/run_all_tests.pl $(CPPGM_DRIVER_APP) my $(DEBUGINFO_DRIVER_O1_TEST_ROOT)
	@scripts/compare_results.pl ref my $(DEBUGINFO_DRIVER_O1_TEST_ROOT)
	$(call run_text_course_bucket,--emit-lowir -gline-tables-only -O1,$(CPPGM_DRIVER_APP),$(COURSE_DEBUGINFO_DRIVER_O1_TEST_ROOT))
	@CPPGM_APP_ARGS='--emit-lowir -gline-tables-only -O2' scripts/run_all_tests.pl $(CPPGM_DRIVER_APP) my $(DEBUGINFO_DRIVER_O2_TEST_ROOT)
	@scripts/compare_results.pl ref my $(DEBUGINFO_DRIVER_O2_TEST_ROOT)
	$(call run_text_course_bucket,--emit-lowir -gline-tables-only -O2,$(CPPGM_DRIVER_APP),$(COURSE_DEBUGINFO_DRIVER_O2_TEST_ROOT))
	$(call run_object_roundtrip_bucket,$(OBJECT_ROUNDTRIP_TEST_ROOT),--debuginfo)
	$(call run_object_roundtrip_bucket,$(COURSE_OBJECT_ROUNDTRIP_TEST_ROOT),--debuginfo)

ref-test:
ifneq ($(strip $(TEST)),)
	@$(REF_TEST_RUN)
else
	@CPPGM_APP_ARGS='-O0' scripts/run_all_tests.pl $(LOWIROPT_REF_APP) ref $(O0_TEST_ROOT)
	@CPPGM_APP_ARGS='-O1' scripts/run_all_tests.pl $(LOWIROPT_REF_APP) ref $(O1_TEST_ROOT)
	@CPPGM_APP_ARGS='-O2' scripts/run_all_tests.pl $(LOWIROPT_REF_APP) ref $(O2_TEST_ROOT)
	@CPPGM_APP_ARGS='--emit-lowir -g0 -O1' scripts/run_all_tests.pl $(CPPGM_DRIVER_REF_APP) ref $(DRIVER_O1_TEST_ROOT)
	@CPPGM_APP_ARGS='--emit-lowir -g0 -O2' scripts/run_all_tests.pl $(CPPGM_DRIVER_REF_APP) ref $(DRIVER_O2_TEST_ROOT)
endif

ref-test-debuginfo:
	@CPPGM_APP_ARGS='-O1' scripts/run_all_tests.pl $(LOWIROPT_REF_APP) ref $(DEBUGINFO_O1_TEST_ROOT)
	@CPPGM_APP_ARGS='-O2' scripts/run_all_tests.pl $(LOWIROPT_REF_APP) ref $(DEBUGINFO_O2_TEST_ROOT)
	@CPPGM_APP_ARGS='--emit-lowir -gline-tables-only -O1' scripts/run_all_tests.pl $(CPPGM_DRIVER_REF_APP) ref $(DEBUGINFO_DRIVER_O1_TEST_ROOT)
	@CPPGM_APP_ARGS='--emit-lowir -gline-tables-only -O2' scripts/run_all_tests.pl $(CPPGM_DRIVER_REF_APP) ref $(DEBUGINFO_DRIVER_O2_TEST_ROOT)

REF_TEST_RUN = case "$(TEST)" in \
	$(O0_TEST_ROOT)/*|$(COURSE_O0_TEST_ROOT)/*) CPPGM_APP_ARGS='-O0' scripts/run_all_tests.pl $(LOWIROPT_REF_APP) ref "$(TEST)" ;; \
	$(O1_TEST_ROOT)/*|$(COURSE_O1_TEST_ROOT)/*) CPPGM_APP_ARGS='-O1' scripts/run_all_tests.pl $(LOWIROPT_REF_APP) ref "$(TEST)" ;; \
	$(O2_TEST_ROOT)/*|$(COURSE_O2_TEST_ROOT)/*) CPPGM_APP_ARGS='-O2' scripts/run_all_tests.pl $(LOWIROPT_REF_APP) ref "$(TEST)" ;; \
	$(DRIVER_O1_TEST_ROOT)/*|$(COURSE_DRIVER_O1_TEST_ROOT)/*) CPPGM_APP_ARGS='--emit-lowir -g0 -O1' scripts/run_all_tests.pl $(CPPGM_DRIVER_REF_APP) ref "$(TEST)" ;; \
	$(DRIVER_O2_TEST_ROOT)/*|$(COURSE_DRIVER_O2_TEST_ROOT)/*) CPPGM_APP_ARGS='--emit-lowir -g0 -O2' scripts/run_all_tests.pl $(CPPGM_DRIVER_REF_APP) ref "$(TEST)" ;; \
	*) echo "Usage: make ref-test TEST=$(O0_TEST_ROOT)/<file>.t | $(O1_TEST_ROOT)/<file>.t | $(O2_TEST_ROOT)/<file>.t | $(DRIVER_O1_TEST_ROOT)/<file>.t | $(DRIVER_O2_TEST_ROOT)/<file>.t"; exit 1 ;; \
esac

CHECK_RUN = case "$(TEST)" in \
	$(OBJECT_ROUNDTRIP_TEST_ROOT)/*|$(COURSE_OBJECT_ROUNDTRIP_TEST_ROOT)/*) scripts/run_object_lowir_roundtrip_tests.pl --app "$(CPPGM_DRIVER_APP)" --test "$(TEST)" ;; \
	*) perl ../scripts/run_routed_test_spec.pl "$(TEST)" \
		'$(O0_TEST_ROOT)/*|$(COURSE_O0_TEST_ROOT)/*::CPPGM_APP_ARGS=-O0 scripts/run_all_tests.pl $(LOWIROPT_APP) check {tests}' \
		'$(O1_TEST_ROOT)/*|$(COURSE_O1_TEST_ROOT)/*|$(DEBUGINFO_O1_TEST_ROOT)/*|$(COURSE_DEBUGINFO_O1_TEST_ROOT)/*::CPPGM_APP_ARGS=-O1 scripts/run_all_tests.pl $(LOWIROPT_APP) check {tests}' \
		'$(O2_TEST_ROOT)/*|$(COURSE_O2_TEST_ROOT)/*|$(DEBUGINFO_O2_TEST_ROOT)/*|$(COURSE_DEBUGINFO_O2_TEST_ROOT)/*::CPPGM_APP_ARGS=-O2 scripts/run_all_tests.pl $(LOWIROPT_APP) check {tests}' \
		'$(DRIVER_O1_TEST_ROOT)/*|$(COURSE_DRIVER_O1_TEST_ROOT)/*::CPPGM_APP_ARGS="--emit-lowir -g0 -O1" scripts/run_all_tests.pl $(CPPGM_DRIVER_APP) check {tests}' \
		'$(DRIVER_O2_TEST_ROOT)/*|$(COURSE_DRIVER_O2_TEST_ROOT)/*::CPPGM_APP_ARGS="--emit-lowir -g0 -O2" scripts/run_all_tests.pl $(CPPGM_DRIVER_APP) check {tests}' \
		'$(DEBUGINFO_DRIVER_O1_TEST_ROOT)/*|$(COURSE_DEBUGINFO_DRIVER_O1_TEST_ROOT)/*::CPPGM_APP_ARGS="--emit-lowir -gline-tables-only -O1" scripts/run_all_tests.pl $(CPPGM_DRIVER_APP) check {tests}' \
		'$(DEBUGINFO_DRIVER_O2_TEST_ROOT)/*|$(COURSE_DEBUGINFO_DRIVER_O2_TEST_ROOT)/*::CPPGM_APP_ARGS="--emit-lowir -gline-tables-only -O2" scripts/run_all_tests.pl $(CPPGM_DRIVER_APP) check {tests}' ;; \
esac
CHECK_COMPARE = case "$(TEST)" in \
	$(OBJECT_ROUNDTRIP_TEST_ROOT)/*|$(COURSE_OBJECT_ROUNDTRIP_TEST_ROOT)/*) true ;; \
	*) scripts/compare_results.pl ref check "$(TEST)" ;; \
esac
include ../scripts/pa_run_check_targets.mk

.PHONY: dev-shared-target ref-test ref-test-debuginfo test-debuginfo
../dev/$(TARGET):
	$(MAKE) -C ../dev $(TARGET) CXX=$(CXX) CPPGM_HOST_CXX=$(CPPGM_HOST_CXX)

dev-shared-target:
	@if [ "$(CPPGM_SKIP_DEV_REBUILD)" = "1" ]; then true; else $(MAKE) -C ../dev $(TARGET) cppgm++ CXX=$(CXX) CPPGM_HOST_CXX=$(CPPGM_HOST_CXX); fi

clean:
	-rm -f $(TARGET) $(TARGET)-debug $(OBJ)/release/*.o $(OBJ)/debug/*.o

$(RDEPDIR)/%.d: ;
.PRECIOUS: $(RDEPDIR)/%.d

$(DDEPDIR)/%.d: ;
.PRECIOUS: $(DDEPDIR)/%.d

-include $(addprefix $(RDEPDIR)/,$(addsuffix .d,$(common_obj_basenames)))
-include $(addprefix $(DDEPDIR)/,$(addsuffix .d,$(common_obj_basenames)))
-include $(RDEPDIR)/test_runner_enabled.d
-include $(DDEPDIR)/test_runner_enabled.d
