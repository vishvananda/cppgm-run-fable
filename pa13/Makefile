TARGET = lowir2cy86
CXX ?= g++
CPPGM_HOST_CXX ?= $(CXX)
CPPGM_STDLIB_FLAGS ?=
CPPGM_TEST_APP ?= ../dev/$(TARGET)
REF_TEST_APP ?= $(CPPGM_TEST_APP)-ref
DEBUGINFO_LOWIROPT_REF_APP ?= ../dev/lowiropt-ref
DEBUGINFO_CPPGM_REF_APP ?= ../dev/cppgm++-ref
DEBUGINFO_LOWIR2NATIVE_REF_APP ?= ../dev/lowir2native-ref
TEST_DEPS ?= dev-shared-target
CC_FLAGS = -std=gnu++11 -Wall -O3 $(CPPGM_STDLIB_FLAGS)
SRC = ../dev/src
GENERATED = ../obj/generated
INC = -I$(SRC) -I$(GENERATED)
OBJ = ../obj
VPATH = $(SRC)
RDEPDIR = $(OBJ)/release/.d
DDEPDIR = $(OBJ)/debug/.d
COURSE_TEST_ROOT = $(wildcard course/pa13)
LOCAL_TEST_ROOT = tests/spec
REF_TEST_ROOT = $(LOCAL_TEST_ROOT)
DEBUGINFO_TEST_ROOT = tests/debuginfo
DEBUGINFO_SOURCE_G0_TEST_ROOT = $(DEBUGINFO_TEST_ROOT)/source-g0
DEBUGINFO_MACHINE_TEST_ROOT = $(DEBUGINFO_TEST_ROOT)/machine
DEBUGINFO_OBJECT_G0_TEST_ROOT = $(DEBUGINFO_TEST_ROOT)/object-g0
DEBUGINFO_OBJECT_TEST_ROOT = $(DEBUGINFO_TEST_ROOT)/object
DEBUGINFO_OBJECT_O1_TEST_ROOT = $(DEBUGINFO_TEST_ROOT)/object-o1
DEBUGINFO_OBJECT_O2_TEST_ROOT = $(DEBUGINFO_TEST_ROOT)/object-o2
DEBUGINFO_EXECUTABLE_G0_TEST_ROOT = $(DEBUGINFO_TEST_ROOT)/executable-g0
DEBUGINFO_EXECUTABLE_MULTI_TEST_ROOT = $(DEBUGINFO_TEST_ROOT)/executable-multi
DEBUGINFO_EXECUTABLE_TEST_ROOT = $(DEBUGINFO_TEST_ROOT)/executable
DEBUGINFO_EXECUTABLE_O1_TEST_ROOT = $(DEBUGINFO_TEST_ROOT)/executable-o1
DEBUGINFO_EXECUTABLE_O2_TEST_ROOT = $(DEBUGINFO_TEST_ROOT)/executable-o2

$(shell mkdir -p $(RDEPDIR) $(DDEPDIR) >/dev/null)

include ../dev/frontend_source_sets.mk

common_obj_basenames = $(FRONTEND_OBJ_BASENAMES_$(TARGET))
common_objs = $(addprefix $(OBJ)/$(1)/,$(addsuffix .o,$(common_obj_basenames)))

all: dev-shared-target

$(OBJ)/release $(OBJ)/debug: %:
	@- mkdir -p $@

$(OBJ)/release/%.o: %.cpp
	@mkdir -p $(@D) $(dir $(RDEPDIR)/$*)
	$(CXX) $(CC_FLAGS) $(INC) -c -MT $@ -MMD -MP -MF $(RDEPDIR)/$*.Td -o $@ $<
	mv -f $(RDEPDIR)/$*.Td $(RDEPDIR)/$*.d

$(OBJ)/debug/%.o: %.cpp
	@mkdir -p $(@D) $(dir $(DDEPDIR)/$*)
	$(CXX) $(CC_FLAGS) $(INC) -c -MT $@ -MMD -MP -MF $(DDEPDIR)/$*.Td -o $@ $<
	mv -f $(DDEPDIR)/$*.Td $(DDEPDIR)/$*.d

ifeq ($(COURSE_TEST_ROOT),)
test: $(TEST_DEPS)
	@scripts/run_all_tests.pl $(CPPGM_TEST_APP) my $(LOCAL_TEST_ROOT)
	@scripts/compare_results.pl ref my $(LOCAL_TEST_ROOT)
else
test: $(TEST_DEPS)
	@scripts/run_all_tests.pl $(CPPGM_TEST_APP) my $(LOCAL_TEST_ROOT)
	@scripts/compare_results.pl ref my $(LOCAL_TEST_ROOT)
	@scripts/run_all_tests.pl $(CPPGM_TEST_APP) my $(COURSE_TEST_ROOT)
	@scripts/compare_results.pl ref my $(COURSE_TEST_ROOT)
endif

ref-test:
	@$(REF_TEST_RUN)

ref-test-debuginfo:
	@CPPGM_LOWIROPT_APP='$(DEBUGINFO_LOWIROPT_REF_APP)' scripts/run_all_tests.pl scripts/lowiropt-o0.sh ref $(DEBUGINFO_TEST_ROOT)/o0
	@CPPGM_CPPGM_APP='$(DEBUGINFO_CPPGM_REF_APP)' scripts/run_all_tests.pl scripts/cppgm-emit-lowir-g0.sh ref $(DEBUGINFO_SOURCE_G0_TEST_ROOT)
	@CPPGM_CPPGM_APP='$(DEBUGINFO_CPPGM_REF_APP)' scripts/run_all_tests.pl scripts/cppgm-emit-lowir-o0.sh ref $(DEBUGINFO_TEST_ROOT)/source
	@CPPGM_LOWIR2NATIVE_APP='$(DEBUGINFO_LOWIR2NATIVE_REF_APP)' scripts/run_all_tests.pl scripts/lowir2native-dump-machine-ir.sh ref $(DEBUGINFO_MACHINE_TEST_ROOT)
	@CPPGM_CPPGM_APP='$(DEBUGINFO_CPPGM_REF_APP)' scripts/run_all_tests.pl scripts/cppgm-dwarfdump-line-g0.sh ref $(DEBUGINFO_OBJECT_G0_TEST_ROOT)
	@CPPGM_CPPGM_APP='$(DEBUGINFO_CPPGM_REF_APP)' scripts/run_all_tests.pl scripts/cppgm-dwarfdump-line-o0.sh ref $(DEBUGINFO_OBJECT_TEST_ROOT)
	@CPPGM_CPPGM_APP='$(DEBUGINFO_CPPGM_REF_APP)' scripts/run_all_tests.pl scripts/cppgm-dwarfdump-line-o1.sh ref $(DEBUGINFO_OBJECT_O1_TEST_ROOT)
	@CPPGM_CPPGM_APP='$(DEBUGINFO_CPPGM_REF_APP)' scripts/run_all_tests.pl scripts/cppgm-dwarfdump-line-o2.sh ref $(DEBUGINFO_OBJECT_O2_TEST_ROOT)
	@CPPGM_CPPGM_APP='$(DEBUGINFO_CPPGM_REF_APP)' scripts/run_all_tests.pl scripts/cppgm-lldb-line-g0.sh ref $(DEBUGINFO_EXECUTABLE_G0_TEST_ROOT)
	@CPPGM_CPPGM_APP='$(DEBUGINFO_CPPGM_REF_APP)' scripts/run_all_tests.pl scripts/cppgm-lldb-multi-o0.sh ref $(DEBUGINFO_EXECUTABLE_MULTI_TEST_ROOT)
	@CPPGM_CPPGM_APP='$(DEBUGINFO_CPPGM_REF_APP)' scripts/run_all_tests.pl scripts/cppgm-lldb-line-o0.sh ref $(DEBUGINFO_EXECUTABLE_TEST_ROOT)
	@CPPGM_CPPGM_APP='$(DEBUGINFO_CPPGM_REF_APP)' scripts/run_all_tests.pl scripts/cppgm-lldb-line-o1.sh ref $(DEBUGINFO_EXECUTABLE_O1_TEST_ROOT)
	@CPPGM_CPPGM_APP='$(DEBUGINFO_CPPGM_REF_APP)' scripts/run_all_tests.pl scripts/cppgm-lldb-line-o2.sh ref $(DEBUGINFO_EXECUTABLE_O2_TEST_ROOT)

include ../scripts/pa_run_check_targets.mk

.PHONY: dev-shared-target debuginfo-lowiropt-target debuginfo-cppgm-target debuginfo-lowir2native-target ref-test ref-test-debuginfo
../dev/$(TARGET):
	$(MAKE) -C ../dev $(TARGET) CXX=$(CXX) CPPGM_HOST_CXX=$(CPPGM_HOST_CXX)

dev-shared-target:
	@if [ "$(CPPGM_SKIP_DEV_REBUILD)" = "1" ]; then true; else $(MAKE) -C ../dev $(TARGET) CXX=$(CXX) CPPGM_HOST_CXX=$(CPPGM_HOST_CXX); fi

debuginfo-lowiropt-target:
	@if [ "$(CPPGM_SKIP_DEV_REBUILD)" = "1" ] && [ -x ../dev/lowiropt ]; then true; else $(MAKE) -C ../dev lowiropt CXX=$(CXX) CPPGM_HOST_CXX=$(CPPGM_HOST_CXX); fi

debuginfo-cppgm-target:
	@if [ "$(CPPGM_SKIP_DEV_REBUILD)" = "1" ] && [ -x ../dev/cppgm++ ]; then true; else $(MAKE) -C ../dev cppgm++ CXX=$(CXX) CPPGM_HOST_CXX=$(CPPGM_HOST_CXX); fi

debuginfo-lowir2native-target:
	@if [ "$(CPPGM_SKIP_DEV_REBUILD)" = "1" ] && [ -x ../dev/lowir2native ]; then true; else $(MAKE) -C ../dev lowir2native CXX=$(CXX) CPPGM_HOST_CXX=$(CPPGM_HOST_CXX); fi

test-debuginfo: debuginfo-lowiropt-target debuginfo-cppgm-target debuginfo-lowir2native-target
	@rm -f .test_failed
	@scripts/run_all_tests.pl scripts/lowiropt-o0.sh my $(DEBUGINFO_TEST_ROOT)/o0
	@KEEP_GOING=1 scripts/compare_results.pl ref my $(DEBUGINFO_TEST_ROOT)/o0
	@scripts/run_all_tests.pl scripts/cppgm-emit-lowir-g0.sh my $(DEBUGINFO_SOURCE_G0_TEST_ROOT)
	@KEEP_GOING=1 scripts/compare_results.pl ref my $(DEBUGINFO_SOURCE_G0_TEST_ROOT)
	@scripts/run_all_tests.pl scripts/cppgm-emit-lowir-o0.sh my $(DEBUGINFO_TEST_ROOT)/source
	@KEEP_GOING=1 scripts/compare_results.pl ref my $(DEBUGINFO_TEST_ROOT)/source
	@scripts/run_all_tests.pl scripts/lowir2native-dump-machine-ir.sh my $(DEBUGINFO_MACHINE_TEST_ROOT)
	@KEEP_GOING=1 scripts/compare_results.pl ref my $(DEBUGINFO_MACHINE_TEST_ROOT)
	@scripts/run_all_tests.pl scripts/cppgm-dwarfdump-line-g0.sh my $(DEBUGINFO_OBJECT_G0_TEST_ROOT)
	@KEEP_GOING=1 scripts/compare_results.pl ref my $(DEBUGINFO_OBJECT_G0_TEST_ROOT)
	@scripts/run_all_tests.pl scripts/cppgm-dwarfdump-line-o0.sh my $(DEBUGINFO_OBJECT_TEST_ROOT)
	@KEEP_GOING=1 scripts/compare_results.pl ref my $(DEBUGINFO_OBJECT_TEST_ROOT)
	@scripts/run_all_tests.pl scripts/cppgm-dwarfdump-line-o1.sh my $(DEBUGINFO_OBJECT_O1_TEST_ROOT)
	@KEEP_GOING=1 scripts/compare_results.pl ref my $(DEBUGINFO_OBJECT_O1_TEST_ROOT)
	@scripts/run_all_tests.pl scripts/cppgm-dwarfdump-line-o2.sh my $(DEBUGINFO_OBJECT_O2_TEST_ROOT)
	@KEEP_GOING=1 scripts/compare_results.pl ref my $(DEBUGINFO_OBJECT_O2_TEST_ROOT)
	@scripts/run_all_tests.pl scripts/cppgm-lldb-line-g0.sh my $(DEBUGINFO_EXECUTABLE_G0_TEST_ROOT)
	@KEEP_GOING=1 scripts/compare_results.pl ref my $(DEBUGINFO_EXECUTABLE_G0_TEST_ROOT)
	@scripts/run_all_tests.pl scripts/cppgm-lldb-multi-o0.sh my $(DEBUGINFO_EXECUTABLE_MULTI_TEST_ROOT)
	@KEEP_GOING=1 scripts/compare_results.pl ref my $(DEBUGINFO_EXECUTABLE_MULTI_TEST_ROOT)
	@scripts/run_all_tests.pl scripts/cppgm-lldb-line-o0.sh my $(DEBUGINFO_EXECUTABLE_TEST_ROOT)
	@KEEP_GOING=1 scripts/compare_results.pl ref my $(DEBUGINFO_EXECUTABLE_TEST_ROOT)
	@scripts/run_all_tests.pl scripts/cppgm-lldb-line-o1.sh my $(DEBUGINFO_EXECUTABLE_O1_TEST_ROOT)
	@KEEP_GOING=1 scripts/compare_results.pl ref my $(DEBUGINFO_EXECUTABLE_O1_TEST_ROOT)
	@scripts/run_all_tests.pl scripts/cppgm-lldb-line-o2.sh my $(DEBUGINFO_EXECUTABLE_O2_TEST_ROOT)
	@KEEP_GOING=1 scripts/compare_results.pl ref my $(DEBUGINFO_EXECUTABLE_O2_TEST_ROOT)
	@test ! -f .test_failed

clean:
	-rm -f $(TARGET) $(TARGET)-debug $(OBJ)/release/*.o $(OBJ)/debug/*.o

$(RDEPDIR)/%.d: ;
.PRECIOUS: $(RDEPDIR)/%.d

$(DDEPDIR)/%.d: ;
.PRECIOUS: $(DDEPDIR)/%.d

-include $(addprefix $(RDEPDIR)/,$(addsuffix .d,$(common_obj_basenames)))
-include $(addprefix $(DDEPDIR)/,$(addsuffix .d,$(common_obj_basenames)))
