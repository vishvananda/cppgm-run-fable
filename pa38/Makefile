TARGET = lowir2native
LOWIR2NATIVE_APP ?= ../dev/$(TARGET)
LOWIR2NATIVE_REF_APP ?= $(LOWIR2NATIVE_APP)-ref
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
REPORT_EMPTY_OPTIONAL_TEST_ROOTS ?= 0
CC_FLAGS = -std=gnu++11 -Wall -O3 $(CPPGM_STDLIB_FLAGS)
SRC = ../dev/src
GENERATED = ../obj/generated
INC = -I$(SRC) -I$(GENERATED)
OBJ = ../obj
VPATH = $(SRC)
RDEPDIR = $(OBJ)/release/.d
DDEPDIR = $(OBJ)/debug/.d
O1_TEST_ROOT ?= tests/o1
O2_TEST_ROOT ?= tests/o2
DEBUGINFO_TEST_ROOT ?= tests/debuginfo
DEBUGINFO_O1_TEST_ROOT ?= $(DEBUGINFO_TEST_ROOT)/o1
DEBUGINFO_O2_TEST_ROOT ?= $(DEBUGINFO_TEST_ROOT)/o2
COURSE_O1_TEST_ROOT ?= course/pa38/o1
COURSE_O2_TEST_ROOT ?= course/pa38/o2
COURSE_DEBUGINFO_TEST_ROOT ?= course/pa38/debuginfo
COURSE_DEBUGINFO_O1_TEST_ROOT ?= $(COURSE_DEBUGINFO_TEST_ROOT)/o1
COURSE_DEBUGINFO_O2_TEST_ROOT ?= $(COURSE_DEBUGINFO_TEST_ROOT)/o2

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

define run_native_bucket
	@if [ -d "$(1)" ] && find "$(1)" -type f -name '*.t' | grep -q .; then \
		CPPGM_APP_ARGS='$(2)' perl ../scripts/run_lowir_native_tests_worker.pl $(LOWIR2NATIVE_APP) my $(1); \
		scripts/compare_results.pl ref my $(1); \
	else \
		if [ "$(REPORT_EMPTY_OPTIONAL_TEST_ROOTS)" = "1" ]; then echo "$(TARGET) $(1): no tests"; fi; \
	fi
endef

define run_native_bucket_ref
	@if [ -d "$(1)" ] && find "$(1)" -type f -name '*.t' | grep -q .; then \
		CPPGM_APP_ARGS='$(2)' perl ../scripts/run_lowir_native_tests_worker.pl $(LOWIR2NATIVE_REF_APP) ref $(1) && \
		perl ../scripts/canonicalize_lowir_native_refs.pl $(1); \
	else \
		if [ "$(REPORT_EMPTY_OPTIONAL_TEST_ROOTS)" = "1" ]; then echo "$(TARGET) $(1): no tests"; fi; \
	fi
endef

test: $(TEST_DEPS)
	$(call run_native_bucket,$(O1_TEST_ROOT),-O1)
	$(call run_native_bucket,$(COURSE_O1_TEST_ROOT),-O1)
	$(call run_native_bucket,$(O2_TEST_ROOT),-O2)
	$(call run_native_bucket,$(COURSE_O2_TEST_ROOT),-O2)

test-debuginfo: $(TEST_DEPS)
	$(call run_native_bucket,$(DEBUGINFO_O1_TEST_ROOT),-O1)
	$(call run_native_bucket,$(COURSE_DEBUGINFO_O1_TEST_ROOT),-O1)
	$(call run_native_bucket,$(DEBUGINFO_O2_TEST_ROOT),-O2)
	$(call run_native_bucket,$(COURSE_DEBUGINFO_O2_TEST_ROOT),-O2)

ref-test:
ifneq ($(strip $(TEST)),)
	@$(REF_TEST_RUN)
else
	$(call run_native_bucket_ref,$(O1_TEST_ROOT),-O1)
	$(call run_native_bucket_ref,$(O2_TEST_ROOT),-O2)
endif

ref-test-debuginfo:
	$(call run_native_bucket_ref,$(DEBUGINFO_O1_TEST_ROOT),-O1)
	$(call run_native_bucket_ref,$(DEBUGINFO_O2_TEST_ROOT),-O2)

REF_TEST_RUN = case "$(TEST)" in \
	$(O1_TEST_ROOT)/*|$(COURSE_O1_TEST_ROOT)/*) CPPGM_APP_ARGS='-O1' perl ../scripts/run_lowir_native_tests_worker.pl $(LOWIR2NATIVE_REF_APP) ref "$(TEST)" && perl ../scripts/canonicalize_lowir_native_refs.pl "$(TEST)" ;; \
	$(O2_TEST_ROOT)/*|$(COURSE_O2_TEST_ROOT)/*) CPPGM_APP_ARGS='-O2' perl ../scripts/run_lowir_native_tests_worker.pl $(LOWIR2NATIVE_REF_APP) ref "$(TEST)" && perl ../scripts/canonicalize_lowir_native_refs.pl "$(TEST)" ;; \
	*) echo "Usage: make ref-test TEST=$(O1_TEST_ROOT)/<file>.t | $(O2_TEST_ROOT)/<file>.t"; exit 1 ;; \
esac

CHECK_RUN = perl ../scripts/run_routed_test_spec.pl "$(TEST)" \
	'$(O1_TEST_ROOT)/*|$(COURSE_O1_TEST_ROOT)/*|$(DEBUGINFO_O1_TEST_ROOT)/*|$(COURSE_DEBUGINFO_O1_TEST_ROOT)/*::CPPGM_APP_ARGS=-O1 scripts/run_all_tests.pl $(LOWIR2NATIVE_APP) check {tests}' \
	'$(O2_TEST_ROOT)/*|$(COURSE_O2_TEST_ROOT)/*|$(DEBUGINFO_O2_TEST_ROOT)/*|$(COURSE_DEBUGINFO_O2_TEST_ROOT)/*::CPPGM_APP_ARGS=-O2 scripts/run_all_tests.pl $(LOWIR2NATIVE_APP) check {tests}'
include ../scripts/pa_run_check_targets.mk

.PHONY: dev-shared-target ref-test ref-test-debuginfo test-debuginfo
../dev/$(TARGET):
	$(MAKE) -C ../dev $(TARGET) CXX=$(CXX) CPPGM_HOST_CXX=$(CPPGM_HOST_CXX)

dev-shared-target:
	@if [ "$(CPPGM_SKIP_DEV_REBUILD)" = "1" ]; then true; else $(MAKE) -C ../dev $(TARGET) CXX=$(CXX) CPPGM_HOST_CXX=$(CPPGM_HOST_CXX); fi

clean:
	-rm -f $(TARGET) $(TARGET)-debug $(OBJ)/release/*.o $(OBJ)/debug/*.o

$(RDEPDIR)/%.d: ;
.PRECIOUS: $(RDEPDIR)/%.d

$(DDEPDIR)/%.d: ;
.PRECIOUS: $(DDEPDIR)/%.d

-include $(addprefix $(RDEPDIR)/,$(addsuffix .d,$(common_obj_basenames)))
-include $(addprefix $(DDEPDIR)/,$(addsuffix .d,$(common_obj_basenames)))
