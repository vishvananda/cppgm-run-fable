TARGET = lowir2native
CXX = g++
CPPGM_STDLIB_FLAGS ?=
CPPGM_TEST_APP ?= ../dev/$(TARGET)
REF_TEST_APP ?= $(CPPGM_TEST_APP)-ref
TEST_DEPS ?= dev-shared-target
CC_FLAGS = -std=gnu++11 -Wall -O3 $(CPPGM_STDLIB_FLAGS)
SRC = ../dev/src
GENERATED = ../obj/generated
INC = -I$(SRC) -I$(GENERATED)
OBJ = ../obj
VPATH = $(SRC)
RDEPDIR = $(OBJ)/release/.d
DDEPDIR = $(OBJ)/debug/.d
COURSE_TEST_ROOT = $(wildcard course/pa28)

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
ifneq ($(CPPGM_BATCH_TESTS),)
	@perl ../scripts/run_lowir_native_tests_worker.pl $(CPPGM_TEST_APP) my tests
else
	@scripts/run_all_tests.pl $(CPPGM_TEST_APP) my tests
endif
	@scripts/compare_results.pl ref my tests

else
test: $(TEST_DEPS)
ifneq ($(CPPGM_BATCH_TESTS),)
	@perl ../scripts/run_lowir_native_tests_worker.pl $(CPPGM_TEST_APP) my tests
else
	@scripts/run_all_tests.pl $(CPPGM_TEST_APP) my tests
endif
	@scripts/compare_results.pl ref my tests
ifneq ($(CPPGM_BATCH_TESTS),)
	@perl ../scripts/run_lowir_native_tests_worker.pl $(CPPGM_TEST_APP) my $(COURSE_TEST_ROOT)
else
	@scripts/run_all_tests.pl $(CPPGM_TEST_APP) my $(COURSE_TEST_ROOT)
endif
	@scripts/compare_results.pl ref my $(COURSE_TEST_ROOT)
endif

ref-test:
	@$(REF_TEST_RUN)
	@if [ -d tests/structural ]; then perl ../scripts/canonicalize_lowir_native_refs.pl tests/structural; fi

include ../scripts/pa_run_check_targets.mk

.PHONY: dev-shared-target ref-test
$(CPPGM_TEST_APP):
	$(MAKE) -C ../dev $(TARGET) CXX=$(CXX) OBJ=$(OBJ)

dev-shared-target:
	@if [ "$(CPPGM_SKIP_DEV_REBUILD)" = "1" ]; then true; else $(MAKE) -C ../dev $(TARGET) CXX=$(CXX) OBJ=$(OBJ); fi

clean:
	-rm -f $(TARGET) $(TARGET)-debug $(OBJ)/release/*.o $(OBJ)/debug/*.o

$(RDEPDIR)/%.d: ;
.PRECIOUS: $(RDEPDIR)/%.d

$(DDEPDIR)/%.d: ;
.PRECIOUS: $(DDEPDIR)/%.d

-include $(addprefix $(RDEPDIR)/,$(addsuffix .d,$(common_obj_basenames)))
-include $(addprefix $(DDEPDIR)/,$(addsuffix .d,$(common_obj_basenames)))
