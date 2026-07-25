TARGETS = \
	pptoken-self \
	posttoken-self \
	ctrlexpr-self \
	macro-self \
	preproc-self \
	recog-self \
	nsdecl-self \
	nsinit-self \
	cy86-self \
	abimangle-self \
	lowiropt-self \
	lowir2cy86-self \
	lowir2native-self \
	cppgm++-self

DEFAULT_BUILD_JOBS = $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
INCEPTION_DEFAULT_BUILD_JOB_CAP ?= 8
INCEPTION_BUILD_JOBS ?= $(shell \
	jobs='$(DEFAULT_BUILD_JOBS)'; cap='$(INCEPTION_DEFAULT_BUILD_JOB_CAP)'; \
	if [ "$$cap" -gt 0 ] 2>/dev/null && [ "$$jobs" -gt "$$cap" ] 2>/dev/null; then echo "$$cap"; else echo "$$jobs"; fi)
INCEPTION_BUILD_MISSING_FIRST ?= 1
HOST_UNAME_S = $(shell uname -s)
CPPGM_EXPLICIT_MAKE_JOB_FLAGS := $(filter -j% --jobs=%,$(MAKEFLAGS))
ifeq ($(findstring -j,$(MAKEFLAGS)),)
MAKEFLAGS += -j$(DEFAULT_BUILD_JOBS)
endif
ifeq ($(MAKELEVEL),0)
CPPGM_MAKE_JOB_FLAGS := $(CPPGM_EXPLICIT_MAKE_JOB_FLAGS)
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


ifeq ($(origin CXX), default)
CXX := ../dev/cppgm++
endif

ifeq ($(origin CPPGM_HOST_CXX), undefined)
ifeq ($(abspath $(CXX)),$(abspath ../dev/cppgm++))
	CPPGM_HOST_CXX := g++
else
	CPPGM_HOST_CXX := $(CXX)
endif
endif

CPPGM_TEST_RUNNER ?= 1
TEST_RUNNER_SHARED_FLAGS = $(if $(filter 1,$(CPPGM_TEST_RUNNER)),-DTEST_RUNNER_ENABLE,)
TEST_RUNNER_ENTRY_FLAGS = $(if $(filter 1,$(CPPGM_TEST_RUNNER)),-Dmain=test_runner_real_main,)
CPPGM_STDLIB_FLAGS ?=
CPPGM_TEXT_TEST_TIMEOUT_SEC ?= 30
INCEPTION_SELFHOST_COMPILE_TIMEOUT_SEC ?= 900
INCEPTION_INCEPTION_COMPILE_TIMEOUT_SEC ?= 3600
INCEPTION_COMPILE_TIMEOUT_SEC ?= $(if $(filter inception,$(INCEPTION_COMPILER_FLAVOR)),$(INCEPTION_INCEPTION_COMPILE_TIMEOUT_SEC),$(if $(filter selfhost,$(INCEPTION_COMPILER_FLAVOR)),$(INCEPTION_SELFHOST_COMPILE_TIMEOUT_SEC),0))
export CPPGM_TEST_JOBS
export CPPGM_HOST_CXX
export CPPGM_STDLIB_FLAGS
export CPPGM_TEXT_TEST_TIMEOUT_SEC
INCEPTION_SELFHOST_OPT_LEVEL ?= 3
INCEPTION_OBJ_ROOT_BASE ?= ../obj/pa39
INCEPTION_BIN_ROOT_BASE ?= $(INCEPTION_OBJ_ROOT_BASE)/bin
INCEPTION_GENERATED ?= $(INCEPTION_OBJ_ROOT_BASE)/generated
INCEPTION_ENTRY_ROOT ?= ../dev
INCEPTION_SOURCE_ROOT ?= $(INCEPTION_ENTRY_ROOT)/src
INCEPTION_BUILTIN_HOST_CONFIG = $(INCEPTION_GENERATED)/cppgm_builtin_host_config.h
INCEPTION_INCLUDE_PATHS := $(INCEPTION_SOURCE_ROOT) $(INCEPTION_GENERATED)
INCEPTION_DEFINES ?=
HOST_CXX_DEFAULT_DEF = -DCPPGM_DEFAULT_HOST_CXX='"$(CPPGM_HOST_CXX)"'
OBJECT_ROOT_DEFAULT_DEF = -DCPPGM_DEFAULT_OBJECT_ROOT='"$(abspath $(INCEPTION_OBJ_ROOT_BASE))"'
INCEPTION_CC_FLAGS = -std=gnu++11 -Wall -O$(INCEPTION_SELFHOST_OPT_LEVEL) $(CPPGM_STDLIB_FLAGS) $(HOST_CXX_DEFAULT_DEF) $(OBJECT_ROOT_DEFAULT_DEF)
INCEPTION_ENTRY_CC_FLAGS = $(INCEPTION_CC_FLAGS) $(TEST_RUNNER_ENTRY_FLAGS)
INCEPTION_LINKER_DETERMINISM_FLAGS =
INCEPTION_LINK_FLAGS = -std=gnu++11 $(CPPGM_STDLIB_FLAGS) $(INCEPTION_LINKER_DETERMINISM_FLAGS)
include ../dev/frontend_source_sets.mk

FRONTEND_TEST_RUNNER_SOURCE_ID ?= test_runner
FRONTEND_BUILTIN_CONFIG_SOURCE_ID ?= preprocessor

ifeq ($(abspath $(CXX)),$(abspath ../dev/cppgm++))
INCEPTION_COMPILER_FLAVOR ?= selfhost
else
INCEPTION_COMPILER_FLAVOR ?= host
endif
INCEPTION_OBJECT_BUILD_JOBS ?= $(if $(filter inception,$(INCEPTION_COMPILER_FLAVOR)),$(INCEPTION_BUILD_JOBS),$(DEFAULT_BUILD_JOBS))

ifneq ($(filter ./% ../% /%,$(CXX)),)
INCEPTION_CXX_DEP := $(CXX)
else
INCEPTION_CXX_DEP :=
endif

INCEPTION_OBJ_ROOT ?= $(INCEPTION_OBJ_ROOT_BASE)/$(INCEPTION_COMPILER_FLAVOR)
INCEPTION_BIN_ROOT ?= $(INCEPTION_BIN_ROOT_BASE)/$(INCEPTION_COMPILER_FLAVOR)
INCEPTION_OUTPUT_SUFFIX ?= -self
INCEPTION_SELFHOST_OBJ_ROOT ?= $(INCEPTION_OBJ_ROOT_BASE)/selfhost
INCEPTION_COMPARE_OBJECTS ?= $(if $(filter inception,$(INCEPTION_COMPILER_FLAVOR)),$(if $(wildcard $(INCEPTION_SELFHOST_OBJ_ROOT)),1,0),0)
PROBE_CXX ?= ../dev/cppgm++
PROBE_TARGET ?= cppgm++
PROBE_OBJ_ROOT_BASE ?= $(INCEPTION_OBJ_ROOT_BASE)/probe
PROBE_OBJ_ROOT ?= $(PROBE_OBJ_ROOT_BASE)/selfhost
PROBE_BIN_ROOT ?= $(PROBE_OBJ_ROOT_BASE)/bin/selfhost
PROBE_SHARED_OBJDIR ?= $(PROBE_OBJ_ROOT)/shared/release
PROBE_SHARED_DEPDIR ?= $(PROBE_SHARED_OBJDIR)/.d
PROBE_CANON_OBJ_ROOT ?= $(INCEPTION_OBJ_ROOT_BASE)/selfhost
PROBE_TIMEOUT_SEC ?= $(INCEPTION_SELFHOST_COMPILE_TIMEOUT_SEC)
PROBE_LINK_OUTPUT ?= $(PROBE_BIN_ROOT)/$(PROBE_TARGET)-probe

CHECKPOINTS = pptoken posttoken ctrlexpr macro preproc recog nsdecl nsinit cy86 cppgm++ abimangle lowiropt lowir2cy86 lowir2native
TEST_TARGETS = $(addprefix test-,$(CHECKPOINTS))
THROUGH_TARGETS = $(addprefix through-,$(CHECKPOINTS))
TEST_THROUGH_TARGETS = $(addprefix test-through-,$(CHECKPOINTS))
INCEPTION_TARGETS = $(addsuffix -inception,$(CHECKPOINTS))
INCEPTION_COMPARE_TARGETS = $(addprefix compare-,$(INCEPTION_TARGETS))
PA_STAGE_NAMES = pa1 pa2 pa3 pa4 pa5 pa6 pa7 pa8 pa9 pa10 pa11 pa12 pa13 pa14 pa15 pa16 pa17 pa18 pa19 pa20 pa21 pa22 pa23 pa24 pa25 pa26 pa27 pa28 pa29 pa30 pa31 pa32 pa33 pa34 pa35 pa36 pa37 pa38
INCEPTION_BATCH_TEST_STAGE_NAMES = pa10 pa11 pa12 pa13 pa14 pa15 pa16 pa17 pa18 pa19 pa20 pa21 pa22 pa23 pa24 pa25 pa26 pa27 pa28 pa29 pa30 pa31 pa32 pa33 pa34 pa35 pa36 pa37 pa38
INCEPTION_BATCH_TEST_ENV = $(if $(filter 1,$(CPPGM_TEST_RUNNER)),CPPGM_BATCH_TESTS=1 WRAPPED_BATCH_STDIN=1,)
inception_stage_test_env = $(if $(filter $(1),$(INCEPTION_BATCH_TEST_STAGE_NAMES)),$(INCEPTION_BATCH_TEST_ENV),)
PA_THROUGH_TARGETS = $(addprefix through-,$(PA_STAGE_NAMES))
PA_TEST_THROUGH_TARGETS = $(addprefix test-through-,$(PA_STAGE_NAMES))
PA_TEST_TARGETS = $(addprefix test-,$(PA_STAGE_NAMES))
PA_TEST_NOBUILD_TARGETS = $(addsuffix -nobuild,$(PA_TEST_TARGETS))

INCEPTION_PREV_pptoken =
INCEPTION_PREV_posttoken = pptoken
INCEPTION_PREV_ctrlexpr = posttoken
INCEPTION_PREV_macro = ctrlexpr
INCEPTION_PREV_preproc = macro
INCEPTION_PREV_recog = preproc
INCEPTION_PREV_nsdecl = recog
INCEPTION_PREV_nsinit = nsdecl
INCEPTION_PREV_cy86 = nsinit
INCEPTION_PREV_cppgm++ = cy86
INCEPTION_PREV_abimangle = cppgm++
INCEPTION_PREV_lowiropt = cppgm++
INCEPTION_PREV_lowir2cy86 = cppgm++
INCEPTION_PREV_lowir2native = lowir2cy86

INCEPTION_CHECKPOINT_FOR_pa1 = pptoken
INCEPTION_CHECKPOINT_FOR_pa2 = posttoken
INCEPTION_CHECKPOINT_FOR_pa3 = ctrlexpr
INCEPTION_CHECKPOINT_FOR_pa4 = macro
INCEPTION_CHECKPOINT_FOR_pa5 = preproc
INCEPTION_CHECKPOINT_FOR_pa6 = recog
INCEPTION_CHECKPOINT_FOR_pa7 = nsdecl
INCEPTION_CHECKPOINT_FOR_pa8 = nsinit
INCEPTION_CHECKPOINT_FOR_pa9 = cy86
INCEPTION_CHECKPOINT_FOR_pa10 = cppgm++
INCEPTION_CHECKPOINT_FOR_pa11 = cppgm++
INCEPTION_CHECKPOINT_FOR_pa12 = cppgm++
INCEPTION_CHECKPOINT_FOR_pa13 = lowir2cy86
INCEPTION_CHECKPOINT_FOR_pa14 = cppgm++
INCEPTION_CHECKPOINT_FOR_pa15 = cppgm++
INCEPTION_CHECKPOINT_FOR_pa16 = cppgm++
INCEPTION_CHECKPOINT_FOR_pa17 = cppgm++
INCEPTION_CHECKPOINT_FOR_pa18 = cppgm++
INCEPTION_CHECKPOINT_FOR_pa19 = cppgm++
INCEPTION_CHECKPOINT_FOR_pa20 = cppgm++
INCEPTION_CHECKPOINT_FOR_pa21 = cppgm++
INCEPTION_CHECKPOINT_FOR_pa22 = cppgm++
INCEPTION_CHECKPOINT_FOR_pa23 = cppgm++
INCEPTION_CHECKPOINT_FOR_pa24 = cppgm++
INCEPTION_CHECKPOINT_FOR_pa25 = cppgm++
INCEPTION_CHECKPOINT_FOR_pa26 = cppgm++
INCEPTION_CHECKPOINT_FOR_pa27 = cppgm++
INCEPTION_CHECKPOINT_FOR_pa28 = lowir2native
INCEPTION_CHECKPOINT_FOR_pa29 = cppgm++
INCEPTION_CHECKPOINT_FOR_pa30 = abimangle
INCEPTION_CHECKPOINT_FOR_pa31 = cppgm++
INCEPTION_CHECKPOINT_FOR_pa32 = cppgm++
INCEPTION_CHECKPOINT_FOR_pa33 = cppgm++
INCEPTION_CHECKPOINT_FOR_pa34 = cppgm++
INCEPTION_CHECKPOINT_FOR_pa35 = cppgm++
INCEPTION_CHECKPOINT_FOR_pa36 = cppgm++
INCEPTION_CHECKPOINT_FOR_pa37 = lowiropt
INCEPTION_CHECKPOINT_FOR_pa38 = lowir2native
INCEPTION_PRIMARY_STAGE_pptoken = pa1
INCEPTION_PRIMARY_STAGE_posttoken = pa2
INCEPTION_PRIMARY_STAGE_ctrlexpr = pa3
INCEPTION_PRIMARY_STAGE_macro = pa4
INCEPTION_PRIMARY_STAGE_preproc = pa5
INCEPTION_PRIMARY_STAGE_recog = pa6
INCEPTION_PRIMARY_STAGE_nsdecl = pa7
INCEPTION_PRIMARY_STAGE_nsinit = pa8
INCEPTION_PRIMARY_STAGE_cy86 = pa9
INCEPTION_PRIMARY_STAGE_cppgm++ = pa10
INCEPTION_PRIMARY_STAGE_abimangle = pa30
INCEPTION_PRIMARY_STAGE_lowiropt = pa37
INCEPTION_PRIMARY_STAGE_lowir2cy86 = pa13
INCEPTION_PRIMARY_STAGE_lowir2native = pa28

INCEPTION_PREV_STAGE_pa1 =
INCEPTION_PREV_STAGE_pa2 = pa1
INCEPTION_PREV_STAGE_pa3 = pa2
INCEPTION_PREV_STAGE_pa4 = pa3
INCEPTION_PREV_STAGE_pa5 = pa4
INCEPTION_PREV_STAGE_pa6 = pa5
INCEPTION_PREV_STAGE_pa7 = pa6
INCEPTION_PREV_STAGE_pa8 = pa7
INCEPTION_PREV_STAGE_pa9 = pa8
INCEPTION_PREV_STAGE_pa10 = pa9
INCEPTION_PREV_STAGE_pa11 = pa10
INCEPTION_PREV_STAGE_pa12 = pa11
INCEPTION_PREV_STAGE_pa13 = pa12
INCEPTION_PREV_STAGE_pa14 = pa13
INCEPTION_PREV_STAGE_pa15 = pa14
INCEPTION_PREV_STAGE_pa16 = pa15
INCEPTION_PREV_STAGE_pa17 = pa16
INCEPTION_PREV_STAGE_pa18 = pa17
INCEPTION_PREV_STAGE_pa19 = pa18
INCEPTION_PREV_STAGE_pa20 = pa19
INCEPTION_PREV_STAGE_pa21 = pa20
INCEPTION_PREV_STAGE_pa22 = pa21
INCEPTION_PREV_STAGE_pa23 = pa22
INCEPTION_PREV_STAGE_pa24 = pa23
INCEPTION_PREV_STAGE_pa25 = pa24
INCEPTION_PREV_STAGE_pa26 = pa25
INCEPTION_PREV_STAGE_pa27 = pa26
INCEPTION_PREV_STAGE_pa28 = pa27
INCEPTION_PREV_STAGE_pa29 = pa28
INCEPTION_PREV_STAGE_pa30 = pa29
INCEPTION_PREV_STAGE_pa31 = pa30
INCEPTION_PREV_STAGE_pa32 = pa31
INCEPTION_PREV_STAGE_pa33 = pa32
INCEPTION_PREV_STAGE_pa34 = pa33
INCEPTION_PREV_STAGE_pa35 = pa34
INCEPTION_PREV_STAGE_pa36 = pa35
INCEPTION_PREV_STAGE_pa37 = pa36
INCEPTION_PREV_STAGE_pa38 = pa37

INCEPTION_STAGE_ARGS_pa1 =
INCEPTION_STAGE_ARGS_pa2 =
INCEPTION_STAGE_ARGS_pa3 =
INCEPTION_STAGE_ARGS_pa4 =
INCEPTION_STAGE_ARGS_pa5 =
INCEPTION_STAGE_ARGS_pa6 =
INCEPTION_STAGE_ARGS_pa7 =
INCEPTION_STAGE_ARGS_pa8 =
INCEPTION_STAGE_ARGS_pa9 =
INCEPTION_STAGE_ARGS_pa10 = --emit-ast
INCEPTION_STAGE_ARGS_pa11 = --emit-types
INCEPTION_STAGE_ARGS_pa12 = --emit-semantics
INCEPTION_STAGE_ARGS_pa13 =
INCEPTION_STAGE_ARGS_pa14 = --emit-lowir -O0
INCEPTION_STAGE_ARGS_pa15 = --emit-lowir -O0
INCEPTION_STAGE_ARGS_pa16 = --emit-lowir -O0
INCEPTION_STAGE_ARGS_pa17 = --emit-lowir -O0
INCEPTION_STAGE_ARGS_pa18 = --emit-lowir -O0
INCEPTION_STAGE_ARGS_pa19 = --emit-lowir -O0
INCEPTION_STAGE_ARGS_pa20 = --emit-lowir -O0
INCEPTION_STAGE_ARGS_pa21 = --emit-lowir -O0
INCEPTION_STAGE_ARGS_pa22 = --emit-lowir -O0
INCEPTION_STAGE_ARGS_pa23 = --emit-lowir -O0
INCEPTION_STAGE_ARGS_pa24 = --emit-lowir -O0
INCEPTION_STAGE_ARGS_pa25 = --emit-lowir -O0
INCEPTION_STAGE_ARGS_pa26 = --emit-lowir -O0
INCEPTION_STAGE_ARGS_pa27 = --emit-lowir -O0
INCEPTION_STAGE_ARGS_pa28 =
INCEPTION_STAGE_ARGS_pa29 =
INCEPTION_STAGE_ARGS_pa30 =
INCEPTION_STAGE_ARGS_pa31 =
INCEPTION_STAGE_ARGS_pa32 =
INCEPTION_STAGE_ARGS_pa33 =
INCEPTION_STAGE_ARGS_pa34 =
INCEPTION_STAGE_ARGS_pa35 =
INCEPTION_STAGE_ARGS_pa36 =
INCEPTION_STAGE_ARGS_pa37 =
INCEPTION_STAGE_ARGS_pa38 =

INCEPTION_COURSE_STAGE_pa1 = course/pa1
INCEPTION_COURSE_STAGE_pa2 = course/pa2
INCEPTION_COURSE_STAGE_pa3 = course/pa3
INCEPTION_COURSE_STAGE_pa4 = course/pa4
INCEPTION_COURSE_STAGE_pa5 = course/pa5
INCEPTION_COURSE_STAGE_pa6 = course/pa6
INCEPTION_COURSE_STAGE_pa7 = course/pa7
INCEPTION_COURSE_STAGE_pa8 = course/pa8
INCEPTION_COURSE_STAGE_pa9 = course/pa9
INCEPTION_COURSE_STAGE_pa10 = course/pa10
INCEPTION_COURSE_STAGE_pa11 = course/pa11
INCEPTION_COURSE_STAGE_pa12 = course/pa12
INCEPTION_COURSE_STAGE_pa13 = course/pa13
INCEPTION_COURSE_STAGE_pa14 = course/pa14
INCEPTION_COURSE_STAGE_pa15 = course/pa15
INCEPTION_COURSE_STAGE_pa16 = course/pa16
INCEPTION_COURSE_STAGE_pa17 = course/pa17
INCEPTION_COURSE_STAGE_pa18 = course/pa18
INCEPTION_COURSE_STAGE_pa19 = course/pa19
INCEPTION_COURSE_STAGE_pa20 = course/pa20
INCEPTION_COURSE_STAGE_pa21 = course/pa21
INCEPTION_COURSE_STAGE_pa22 = course/pa22
INCEPTION_COURSE_STAGE_pa23 = course/pa23
INCEPTION_COURSE_STAGE_pa24 = course/pa24
INCEPTION_COURSE_STAGE_pa25 = course/pa25
INCEPTION_COURSE_STAGE_pa26 = course/pa26
INCEPTION_COURSE_STAGE_pa27 = course/pa27
INCEPTION_COURSE_STAGE_pa28 = course/pa28
INCEPTION_COURSE_STAGE_pa29 = course/pa29
INCEPTION_COURSE_STAGE_pa30 = course/pa30
INCEPTION_COURSE_STAGE_pa31 = course/pa31
INCEPTION_COURSE_STAGE_pa32 = course/pa32
INCEPTION_COURSE_STAGE_pa33 = course/pa33
INCEPTION_COURSE_STAGE_pa34 = course/pa34
INCEPTION_COURSE_STAGE_pa35 = course/pa35
INCEPTION_COURSE_STAGE_pa36 = course/pa36
INCEPTION_COURSE_STAGE_pa37 = course/pa37
INCEPTION_COURSE_STAGE_pa38 = course/pa38

inception_checkpoint_entry = $(INCEPTION_ENTRY_ROOT)/$(1).cpp
inception_source_path = $(INCEPTION_SOURCE_ROOT)/$(1).cpp
inception_checkpoint_source_ids = $(if $(filter undefined,$(origin FRONTEND_SOURCE_IDS_$(1))),$(FRONTEND_OBJ_BASENAMES_$(1)),$(FRONTEND_SOURCE_IDS_$(1)))
$(foreach checkpoint,$(CHECKPOINTS),$(if $(filter undefined,$(origin FRONTEND_SOURCE_IDS_$(checkpoint))),$(if $(filter undefined,$(origin FRONTEND_OBJ_BASENAMES_$(checkpoint))),$(error missing FRONTEND_SOURCE_IDS_$(checkpoint) or FRONTEND_OBJ_BASENAMES_$(checkpoint) in ../dev/frontend_source_sets.mk),),))
inception_checkpoint_sources = $(foreach source_id,$(call inception_checkpoint_source_ids,$(1)),$(call inception_source_path,$(source_id)))
$(foreach checkpoint,$(CHECKPOINTS),$(eval INCEPTION_ENTRY_$(checkpoint) := $(call inception_checkpoint_entry,$(checkpoint))))
$(foreach checkpoint,$(CHECKPOINTS),$(eval INCEPTION_SOURCES_$(checkpoint) := $(call inception_checkpoint_sources,$(checkpoint))))

INCEPTION_ALL_SHARED_SOURCES := $(sort $(foreach checkpoint,$(CHECKPOINTS),$(INCEPTION_SOURCES_$(checkpoint))))

inception_objdir = $(INCEPTION_OBJ_ROOT)/$(1)/release
inception_depdir = $(call inception_objdir,$(1))/.d
INCEPTION_SHARED_OBJDIR ?= $(INCEPTION_OBJ_ROOT)/shared/release
INCEPTION_SHARED_DEPDIR ?= $(INCEPTION_SHARED_OBJDIR)/.d
inception_entry_stem = $(basename $(notdir $(1)))
inception_shared_id = $(patsubst $(INCEPTION_SOURCE_ROOT)/%.cpp,%,$(1))
inception_entry_suffix = $(if $(filter 1,$(CPPGM_TEST_RUNNER)),-runner,-plain)
inception_runner_suffix = $(if $(filter 1,$(CPPGM_TEST_RUNNER)),-enabled,-disabled)
inception_binary_path = $(INCEPTION_BIN_ROOT)/$(1)$(INCEPTION_OUTPUT_SUFFIX)
inception_link_output_path = $(INCEPTION_BIN_ROOT)/$(1)
inception_self_binary_path = $(INCEPTION_BIN_ROOT_BASE)/selfhost/$(1)-self
inception_inception_binary_path = $(INCEPTION_BIN_ROOT_BASE)/inception/$(1)-inception
inception_selfhost_entry_object_path = $(INCEPTION_SELFHOST_OBJ_ROOT)/$(1)/release/$(call inception_entry_stem,$(INCEPTION_ENTRY_$(1)))$(inception_entry_suffix).o
inception_selfhost_runner_object_path = $(INCEPTION_SELFHOST_OBJ_ROOT)/shared/release/test_runner$(inception_runner_suffix).o
inception_selfhost_shared_object_path = $(INCEPTION_SELFHOST_OBJ_ROOT)/shared/release/$(call inception_shared_id,$(1)).o
INCEPTION_RESTORED_CPPGM_SELF ?= $(call inception_self_binary_path,cppgm++)
inception_entry_object_path = $(call inception_objdir,$(1))/$(call inception_entry_stem,$(INCEPTION_ENTRY_$(1)))$(inception_entry_suffix).o
inception_entry_depfile_path = $(call inception_depdir,$(1))/$(call inception_entry_stem,$(INCEPTION_ENTRY_$(1)))$(inception_entry_suffix).d
inception_runner_object_path = $(INCEPTION_SHARED_OBJDIR)/test_runner$(inception_runner_suffix).o
inception_runner_depfile_path = $(INCEPTION_SHARED_DEPDIR)/test_runner$(inception_runner_suffix).d
inception_shared_object_path = $(INCEPTION_SHARED_OBJDIR)/$(call inception_shared_id,$(1)).o
inception_shared_depfile_path = $(INCEPTION_SHARED_DEPDIR)/$(call inception_shared_id,$(1)).d
inception_object_list = $(foreach src,$(INCEPTION_SOURCES_$(1)),$(call inception_shared_object_path,$(src)))
inception_link_inputs = $(call inception_entry_object_path,$(1)) $(call inception_object_list,$(1)) $(if $(filter 1,$(CPPGM_TEST_RUNNER)),$(inception_runner_object_path))
# Build never-seen objects in a separate recursive pass, but keep final linking
# on inception_link_inputs.
inception_missing_link_inputs = $(strip $(foreach obj,$(call inception_link_inputs,$(1)),$(if $(wildcard $(obj)),,$(obj))))
inception_existing_link_inputs = $(filter-out $(call inception_missing_link_inputs,$(1)),$(call inception_link_inputs,$(1)))
inception_stage_binary = $(call inception_self_binary_path,$(INCEPTION_CHECKPOINT_FOR_$(1)))
INCEPTION_INCEPTION_CXX ?= $(call inception_self_binary_path,cppgm++)

all: $(TARGETS)

test: $(PA_TEST_TARGETS)

ladder: $(TARGETS)

preservation: $(PA_TEST_TARGETS)

inception: $(INCEPTION_TARGETS)

compare-inception: $(INCEPTION_COMPARE_TARGETS)

bitcmp: compare-inception

cppgm++-inception-from-restored-self:
	@if [ ! -x "$(INCEPTION_RESTORED_CPPGM_SELF)" ]; then \
		echo "missing restored self compiler: $(INCEPTION_RESTORED_CPPGM_SELF)" >&2; \
		exit 2; \
	fi
	+env MAKEFLAGS= $(MAKE) -j$(INCEPTION_BUILD_JOBS) "$(call inception_inception_binary_path,cppgm++)" \
		INCEPTION_COMPILER_FLAVOR=inception \
		INCEPTION_COMPARE_OBJECTS=0 \
		INCEPTION_OUTPUT_SUFFIX=-inception \
		INCEPTION_OBJ_ROOT_BASE="$(INCEPTION_OBJ_ROOT_BASE)" \
		INCEPTION_BIN_ROOT_BASE="$(INCEPTION_BIN_ROOT_BASE)" \
		INCEPTION_SELFHOST_OPT_LEVEL="$(INCEPTION_SELFHOST_OPT_LEVEL)" \
		INCEPTION_BUILD_JOBS="$(INCEPTION_BUILD_JOBS)" \
		INCEPTION_BUILD_MISSING_FIRST="$(INCEPTION_BUILD_MISSING_FIRST)" \
		CXX="$(INCEPTION_RESTORED_CPPGM_SELF)" \
		CPPGM_HOST_CXX="$(CPPGM_HOST_CXX)" \
		CPPGM_STDLIB_FLAGS="$(CPPGM_STDLIB_FLAGS)" \
		CPPGM_TEST_RUNNER="$(CPPGM_TEST_RUNNER)" \
		INCEPTION_DEFINES='$(INCEPTION_DEFINES)'
	@ln -sf $(call inception_inception_binary_path,cppgm++) cppgm++-inception

compare-cppgm++-inception-from-restored-self: cppgm++-inception-from-restored-self
	@cmp -s "$(INCEPTION_RESTORED_CPPGM_SELF)" "$(call inception_inception_binary_path,cppgm++)" || \
		(echo "mismatch: $(INCEPTION_RESTORED_CPPGM_SELF) != $(call inception_inception_binary_path,cppgm++)" >&2; exit 1)
	@echo "MATCH cppgm++: $(call inception_inception_binary_path,cppgm++)"

compiler-ready:
	@if [ -x "$(CXX)" ]; then \
		exit 0; \
	fi; \
	if command -v "$(CXX)" >/dev/null 2>&1; then \
		exit 0; \
	fi; \
	if [ ! -x "$(CXX)" ]; then \
		if [ "$(CXX)" = "../dev/cppgm++" ]; then \
			$(MAKE) -C ../dev cppgm++ \
				CXX="$(CPPGM_HOST_CXX)" \
				CPPGM_HOST_CXX="$(CPPGM_HOST_CXX)" \
				CPPGM_TEST_RUNNER="$(CPPGM_TEST_RUNNER)"; \
		else \
			echo "missing compiler: $(CXX)" >&2; \
			exit 1; \
		fi; \
	fi

probe-compiler-ready:
	@if [ -x "$(PROBE_CXX)" ]; then \
		exit 0; \
	fi; \
	if command -v "$(PROBE_CXX)" >/dev/null 2>&1; then \
		exit 0; \
	fi; \
	if [ "$(PROBE_CXX)" = "../dev/cppgm++" ]; then \
		$(MAKE) -C ../dev cppgm++ \
			CXX="$(CPPGM_HOST_CXX)" \
			CPPGM_HOST_CXX="$(CPPGM_HOST_CXX)" \
			CPPGM_TEST_RUNNER="$(CPPGM_TEST_RUNNER)"; \
	else \
		echo "missing probe compiler: $(PROBE_CXX)" >&2; \
		exit 1; \
	fi

probe-self-object: probe-compiler-ready $(INCEPTION_BUILTIN_HOST_CONFIG)
	@if [ -z "$(strip $(SOURCE))" ]; then \
		echo "Usage: make probe-self-object SOURCE=$(INCEPTION_SOURCE_ROOT)/<relative/file>.cpp [PROBE_CXX=../dev/cppgm++]" >&2; \
		exit 2; \
	fi
	@src='$(SOURCE)'; \
	case "$$src" in \
		dev/*) src="../$$src" ;; \
		./dev/*) src="../$${src#./}" ;; \
	esac; \
	if [ ! -f "$$src" ]; then \
		echo "missing source: $$src" >&2; \
		exit 2; \
	fi; \
	case "$$src" in \
		$(call inception_source_path,$(FRONTEND_TEST_RUNNER_SOURCE_ID))) \
			kind=runner; \
			obj="$(PROBE_SHARED_OBJDIR)/test_runner$(inception_runner_suffix).o"; \
			dep="$(PROBE_SHARED_DEPDIR)/test_runner$(inception_runner_suffix).d" ;; \
		$(INCEPTION_SOURCE_ROOT)/*.cpp) \
			kind=shared; \
			source_id=$${src#$(INCEPTION_SOURCE_ROOT)/}; source_id=$${source_id%.cpp}; \
			obj="$(PROBE_SHARED_OBJDIR)/$$source_id.o"; \
			dep="$(PROBE_SHARED_DEPDIR)/$$source_id.d" ;; \
		$(INCEPTION_ENTRY_ROOT)/*.cpp) \
			stem=$$(basename "$$src" .cpp); \
			kind=entry; \
			obj="$(PROBE_OBJ_ROOT)/$$stem/release/$$stem$(inception_entry_suffix).o"; \
			dep="$(PROBE_OBJ_ROOT)/$$stem/release/.d/$$stem$(inception_entry_suffix).d" ;; \
		*) \
			echo "unsupported probe source: $$src" >&2; \
			echo "Use $(INCEPTION_SOURCE_ROOT)/<relative/file>.cpp or $(INCEPTION_ENTRY_ROOT)/<tool>.cpp" >&2; \
			exit 2 ;; \
	esac; \
	mkdir -p "$${obj%/*}" "$${dep%/*}"; \
	echo "PROBE source: $$src"; \
	echo "PROBE compiler: $(PROBE_CXX)"; \
	echo "PROBE object: $$obj"; \
	echo "PROBE depfile: $$dep"; \
	if [ "$$kind" = runner ]; then \
		perl ../scripts/run_with_timeout.pl --timeout-sec "$(PROBE_TIMEOUT_SEC)" --label "probe self compile $$src" -- $(PROBE_CXX) $(INCEPTION_CC_FLAGS) $(TEST_RUNNER_SHARED_FLAGS) $(foreach inc,$(INCEPTION_INCLUDE_PATHS),-I$(inc)) $(foreach def,$(INCEPTION_DEFINES),-D$(def)) -MMD -MP -MF "$$dep" -c -o "$$obj" "$$src"; \
	elif [ "$$kind" = entry ]; then \
		perl ../scripts/run_with_timeout.pl --timeout-sec "$(PROBE_TIMEOUT_SEC)" --label "probe self compile $$src" -- $(PROBE_CXX) $(INCEPTION_ENTRY_CC_FLAGS) $(foreach inc,$(INCEPTION_INCLUDE_PATHS),-I$(inc)) $(foreach def,$(INCEPTION_DEFINES),-D$(def)) -MMD -MP -MF "$$dep" -c -o "$$obj" "$$src"; \
	else \
		perl ../scripts/run_with_timeout.pl --timeout-sec "$(PROBE_TIMEOUT_SEC)" --label "probe self compile $$src" -- $(PROBE_CXX) $(INCEPTION_CC_FLAGS) $(foreach inc,$(INCEPTION_INCLUDE_PATHS),-I$(inc)) $(foreach def,$(INCEPTION_DEFINES),-D$(def)) -MMD -MP -MF "$$dep" -c -o "$$obj" "$$src"; \
	fi

probe-self-link:
	@if [ -z "$(filter $(PROBE_TARGET),$(CHECKPOINTS))" ]; then \
		echo "unknown PROBE_TARGET=$(PROBE_TARGET)" >&2; \
		echo "known targets: $(CHECKPOINTS)" >&2; \
		exit 2; \
	fi
	@src='$(SOURCE)'; \
	case "$$src" in \
		dev/*) src="../$$src" ;; \
		./dev/*) src="../$${src#./}" ;; \
	esac; \
	case "$$src" in \
		$(call inception_source_path,$(FRONTEND_TEST_RUNNER_SOURCE_ID))) \
			probe_obj="$(PROBE_SHARED_OBJDIR)/test_runner$(inception_runner_suffix).o"; \
			canon_obj="$(PROBE_CANON_OBJ_ROOT)/shared/release/test_runner$(inception_runner_suffix).o" ;; \
		$(INCEPTION_SOURCE_ROOT)/*.cpp) \
			source_id=$${src#$(INCEPTION_SOURCE_ROOT)/}; source_id=$${source_id%.cpp}; \
			probe_obj="$(PROBE_SHARED_OBJDIR)/$$source_id.o"; \
			canon_obj="$(PROBE_CANON_OBJ_ROOT)/shared/release/$$source_id.o" ;; \
		$(INCEPTION_ENTRY_ROOT)/*.cpp) \
			stem=$$(basename "$$src" .cpp); \
			probe_obj="$(PROBE_OBJ_ROOT)/$$stem/release/$$stem$(inception_entry_suffix).o"; \
			canon_obj="$(PROBE_CANON_OBJ_ROOT)/$$stem/release/$$stem$(inception_entry_suffix).o" ;; \
		*) \
			echo "unsupported probe source: $$src" >&2; \
			exit 2 ;; \
	esac; \
	mkdir -p "$(PROBE_BIN_ROOT)"; \
	inputs=; replaced=0; \
	for input in $(subst $(INCEPTION_OBJ_ROOT),$(PROBE_CANON_OBJ_ROOT),$(call inception_link_inputs,$(PROBE_TARGET))); do \
		use="$$input"; \
		if [ "$$input" = "$$canon_obj" ]; then \
			use="$$probe_obj"; \
			replaced=1; \
		fi; \
		inputs="$$inputs $$use"; \
	done; \
	if [ "$$replaced" != 1 ]; then \
		echo "probe object $$canon_obj is not part of PROBE_TARGET=$(PROBE_TARGET)" >&2; \
		exit 2; \
	fi; \
	$(MAKE) probe-self-object SOURCE='$(SOURCE)' PROBE_CXX='$(PROBE_CXX)' PROBE_TIMEOUT_SEC='$(PROBE_TIMEOUT_SEC)' || exit $$?; \
	missing=0; \
	for input in $$inputs; do \
		if [ ! -f "$$input" ]; then \
			echo "missing link input: $$input" >&2; \
			missing=1; \
		fi; \
	done; \
	if [ "$$missing" != 0 ]; then \
		echo "Build the canonical self checkpoint first, then rerun this scratch link." >&2; \
		exit 2; \
	fi; \
	echo "PROBE link target: $(PROBE_TARGET)"; \
	echo "PROBE binary: $(PROBE_LINK_OUTPUT)"; \
	$(CPPGM_HOST_CXX) $(INCEPTION_LINK_FLAGS) -I$(INCEPTION_SOURCE_ROOT) -o "$(PROBE_LINK_OUTPUT)" $$inputs

$(INCEPTION_BUILTIN_HOST_CONFIG): FORCE
	@mkdir -p $(dir $@)
	@CPPGM_BUILD_HOST_CXX='$(CPPGM_HOST_CXX)' \
		CPPGM_STDLIB_FLAGS='$(CPPGM_STDLIB_FLAGS)' \
		perl ../dev/gen_builtin_host_config.pl > $@.tmp
	@if ! cmp -s $@.tmp $@ 2>/dev/null; then mv $@.tmp $@; else rm -f $@.tmp; fi

../dev/cppgm++:
	$(MAKE) -C ../dev cppgm++ \
		CXX="$(CPPGM_HOST_CXX)" \
		CPPGM_HOST_CXX="$(CPPGM_HOST_CXX)" \
		CPPGM_TEST_RUNNER="$(CPPGM_TEST_RUNNER)"

define INCEPTION_SOURCE_RULE
$(call inception_shared_object_path,$(1)): $(1) $(INCEPTION_CXX_DEP) $(if $(filter 1,$(INCEPTION_COMPARE_OBJECTS)),$(call inception_selfhost_shared_object_path,$(1))) | compiler-ready $(INCEPTION_SHARED_OBJDIR) $(INCEPTION_SHARED_DEPDIR)
	@mkdir -p $$(@D) $(dir $(call inception_shared_depfile_path,$(1)))
	perl ../scripts/run_with_timeout.pl --timeout-sec "$(INCEPTION_COMPILE_TIMEOUT_SEC)" --label "$(INCEPTION_COMPILER_FLAVOR) compile $$<" -- $(CXX) $(INCEPTION_CC_FLAGS) $(foreach inc,$(INCEPTION_INCLUDE_PATHS),-I$(inc)) $(foreach def,$(INCEPTION_DEFINES),-D$(def)) -MMD -MP -MF $(call inception_shared_depfile_path,$(1)) -c -o $$@ $$<
	@if [ "$(INCEPTION_COMPARE_OBJECTS)" = "1" ]; then \
		if ! cmp -s "$(call inception_selfhost_shared_object_path,$(1))" "$$@"; then \
			echo "object mismatch: $(call inception_selfhost_shared_object_path,$(1)) != $$@" >&2; \
			exit 1; \
		fi; \
		echo "MATCH object: $$@"; \
	fi
endef

$(INCEPTION_BIN_ROOT):
	@mkdir -p $@

$(INCEPTION_SHARED_OBJDIR) $(INCEPTION_SHARED_DEPDIR):
	@mkdir -p $@

$(call inception_shared_object_path,$(call inception_source_path,$(FRONTEND_BUILTIN_CONFIG_SOURCE_ID))): $(INCEPTION_BUILTIN_HOST_CONFIG)

$(inception_runner_object_path): $(call inception_source_path,$(FRONTEND_TEST_RUNNER_SOURCE_ID)) $(INCEPTION_CXX_DEP) $(if $(filter 1,$(INCEPTION_COMPARE_OBJECTS)),$(inception_selfhost_runner_object_path)) | compiler-ready $(INCEPTION_SHARED_OBJDIR) $(INCEPTION_SHARED_DEPDIR)
	@mkdir -p $(@D) $(INCEPTION_SHARED_DEPDIR)
	perl ../scripts/run_with_timeout.pl --timeout-sec "$(INCEPTION_COMPILE_TIMEOUT_SEC)" --label "$(INCEPTION_COMPILER_FLAVOR) compile $<" -- $(CXX) $(INCEPTION_CC_FLAGS) $(TEST_RUNNER_SHARED_FLAGS) $(foreach inc,$(INCEPTION_INCLUDE_PATHS),-I$(inc)) $(foreach def,$(INCEPTION_DEFINES),-D$(def)) -MMD -MP -MF $(inception_runner_depfile_path) -c -o $@ $<
	@if [ "$(INCEPTION_COMPARE_OBJECTS)" = "1" ]; then \
		if ! cmp -s "$(inception_selfhost_runner_object_path)" "$@"; then \
			echo "object mismatch: $(inception_selfhost_runner_object_path) != $@" >&2; \
			exit 1; \
		fi; \
		echo "MATCH object: $@"; \
	fi

define INCEPTION_CHECKPOINT_RULES
$(call inception_objdir,$(1)) $(call inception_depdir,$(1)):
	@mkdir -p $$@

$(call inception_entry_object_path,$(1)): $(INCEPTION_ENTRY_$(1)) $(INCEPTION_CXX_DEP) $(if $(filter 1,$(INCEPTION_COMPARE_OBJECTS)),$(call inception_selfhost_entry_object_path,$(1))) | compiler-ready $(call inception_objdir,$(1)) $(call inception_depdir,$(1))
	@mkdir -p $$(@D) $(call inception_depdir,$(1))
	perl ../scripts/run_with_timeout.pl --timeout-sec "$(INCEPTION_COMPILE_TIMEOUT_SEC)" --label "$(INCEPTION_COMPILER_FLAVOR) compile $$<" -- $(CXX) $(INCEPTION_ENTRY_CC_FLAGS) $(foreach inc,$(INCEPTION_INCLUDE_PATHS),-I$(inc)) $(foreach def,$(INCEPTION_DEFINES),-D$(def)) -MMD -MP -MF $(call inception_entry_depfile_path,$(1)) -c -o $$@ $$<
	@if [ "$(INCEPTION_COMPARE_OBJECTS)" = "1" ]; then \
		if ! cmp -s "$(call inception_selfhost_entry_object_path,$(1))" "$$@"; then \
			echo "object mismatch: $(call inception_selfhost_entry_object_path,$(1)) != $$@" >&2; \
			exit 1; \
		fi; \
		echo "MATCH object: $$@"; \
	fi

$(call inception_binary_path,$(1)): FORCE | $(INCEPTION_BIN_ROOT)
	+@if [ "$(INCEPTION_BUILD_MISSING_FIRST)" = "1" ] && [ -n "$(call inception_missing_link_inputs,$(1))" ]; then \
		echo "Building missing $(1) objects first"; \
		env MAKEFLAGS= $$(MAKE) -j$(INCEPTION_OBJECT_BUILD_JOBS) $(call inception_missing_link_inputs,$(1)); \
	fi
	+@if [ "$(INCEPTION_BUILD_MISSING_FIRST)" = "1" ]; then \
		if [ -n "$(call inception_existing_link_inputs,$(1))" ]; then \
			env MAKEFLAGS= $$(MAKE) -j$(INCEPTION_OBJECT_BUILD_JOBS) $(call inception_existing_link_inputs,$(1)); \
		fi; \
	else \
		env MAKEFLAGS= $$(MAKE) -j$(INCEPTION_OBJECT_BUILD_JOBS) $(call inception_link_inputs,$(1)); \
	fi
	@rm -f "$(call inception_link_output_path,$(1)).tmp"
	$(CPPGM_HOST_CXX) $(INCEPTION_LINK_FLAGS) -I$(INCEPTION_SOURCE_ROOT) -o "$(call inception_link_output_path,$(1)).tmp" $(call inception_link_inputs,$(1))
	@if ! cmp -s "$(call inception_link_output_path,$(1)).tmp" "$$@" 2>/dev/null; then \
		mv -f "$(call inception_link_output_path,$(1)).tmp" "$$@"; \
	else \
		rm -f "$(call inception_link_output_path,$(1)).tmp"; \
	fi
	@touch "$$@.link-stamp"

$(1)-self: $(call inception_binary_path,$(1))
	@ln -sf $(call inception_binary_path,$(1)) $$@

test-$(1): $(1)-self
	$$(MAKE) test-$(INCEPTION_PRIMARY_STAGE_$(1))-nobuild

through-$(1): $(if $(INCEPTION_PREV_$(1)),through-$(INCEPTION_PREV_$(1)))
	$$(MAKE) $(1)-self

test-through-$(1): $(if $(INCEPTION_PREV_$(1)),test-through-$(INCEPTION_PREV_$(1)))
	$$(MAKE) test-$(1)
endef

define INCEPTION_STAGE_RULES
test-$(1): $(INCEPTION_CHECKPOINT_FOR_$(1))-self
	$$(MAKE) test-$(1)-nobuild

test-$(1)-nobuild:
	@if [ ! -x "$(call inception_stage_binary,$(1))" ]; then \
		echo "missing staged binary: $(call inception_stage_binary,$(1))" >&2; \
		echo "build it first with 'make through-$(1)' or 'make $(INCEPTION_CHECKPOINT_FOR_$(1))-self'" >&2; \
		exit 2; \
	fi
	@if [ "$(1)" = "pa37" ]; then \
		$(call inception_stage_test_env,$(1)) $(MAKE) -C ../pa37 \
			CPPGM_SKIP_DEV_REBUILD=1 \
			TEST_DEPS= \
			CXX="$(CPPGM_HOST_CXX)" \
			CPPGM_HOST_CXX="$(CPPGM_HOST_CXX)" \
			CPPGM_STDLIB_FLAGS="$(CPPGM_STDLIB_FLAGS)" \
			CPPGM_TEST_RUNNER="$(CPPGM_TEST_RUNNER)" \
			LOWIROPT_APP="$(call inception_stage_binary,$(1))" \
			CPPGM_DRIVER_APP="$(call inception_binary_path,cppgm++)" \
			test; \
	elif [ "$(1)" = "pa38" ]; then \
		$(call inception_stage_test_env,$(1)) $(MAKE) -C ../pa38 \
			CPPGM_SKIP_DEV_REBUILD=1 \
			TEST_DEPS= \
			CXX="$(CPPGM_HOST_CXX)" \
			CPPGM_HOST_CXX="$(CPPGM_HOST_CXX)" \
			CPPGM_STDLIB_FLAGS="$(CPPGM_STDLIB_FLAGS)" \
			CPPGM_TEST_RUNNER="$(CPPGM_TEST_RUNNER)" \
			LOWIR2NATIVE_APP="$(call inception_stage_binary,$(1))" \
			test; \
	else \
		$(call inception_stage_test_env,$(1)) $(MAKE) -C ../$(1) \
			CPPGM_SKIP_DEV_REBUILD=1 \
			TEST_DEPS= \
			CXX="$(CPPGM_HOST_CXX)" \
			CPPGM_HOST_CXX="$(CPPGM_HOST_CXX)" \
			CPPGM_STDLIB_FLAGS="$(CPPGM_STDLIB_FLAGS)" \
			CPPGM_TEST_RUNNER="$(CPPGM_TEST_RUNNER)" \
			CPPGM_TEST_APP="$(call inception_stage_binary,$(1))" \
			CPPGM_APP_ARGS='$(INCEPTION_STAGE_ARGS_$(1))' \
			test; \
	fi

through-$(1): $(if $(INCEPTION_PREV_STAGE_$(1)),through-$(INCEPTION_PREV_STAGE_$(1)))
	$$(MAKE) $(INCEPTION_CHECKPOINT_FOR_$(1))-self

test-through-$(1): $(if $(INCEPTION_PREV_STAGE_$(1)),test-through-$(INCEPTION_PREV_STAGE_$(1)))
	$$(MAKE) test-$(1)
endef

define INCEPTION_INCEPTION_RULES
$(1)-inception: cppgm++-self $(1)-self
	+env MAKEFLAGS= $$(MAKE) -j$(INCEPTION_BUILD_JOBS) "$(call inception_inception_binary_path,$(1))" \
		INCEPTION_COMPILER_FLAVOR=inception \
		INCEPTION_OUTPUT_SUFFIX=-inception \
		INCEPTION_OBJ_ROOT_BASE="$(INCEPTION_OBJ_ROOT_BASE)" \
		INCEPTION_BIN_ROOT_BASE="$(INCEPTION_BIN_ROOT_BASE)" \
		INCEPTION_SELFHOST_OPT_LEVEL="$(INCEPTION_SELFHOST_OPT_LEVEL)" \
		INCEPTION_BUILD_JOBS="$(INCEPTION_BUILD_JOBS)" \
		INCEPTION_BUILD_MISSING_FIRST="$(INCEPTION_BUILD_MISSING_FIRST)" \
		CXX="$(INCEPTION_INCEPTION_CXX)" \
		CPPGM_HOST_CXX="$(CPPGM_HOST_CXX)" \
		CPPGM_STDLIB_FLAGS="$(CPPGM_STDLIB_FLAGS)" \
		CPPGM_TEST_RUNNER="$(CPPGM_TEST_RUNNER)" \
		INCEPTION_DEFINES='$(INCEPTION_DEFINES)'
	@ln -sf $(call inception_inception_binary_path,$(1)) $$@

compare-$(1)-inception: $(1)-self $(1)-inception
	@cmp -s "$(call inception_self_binary_path,$(1))" "$(call inception_inception_binary_path,$(1))" || \
		(echo "mismatch: $(call inception_self_binary_path,$(1)) != $(call inception_inception_binary_path,$(1))" >&2; exit 1)
	@echo "MATCH $(1): $(call inception_inception_binary_path,$(1))"
endef

$(foreach checkpoint,$(CHECKPOINTS),$(eval $(call INCEPTION_CHECKPOINT_RULES,$(checkpoint))))
$(foreach src,$(INCEPTION_ALL_SHARED_SOURCES),$(eval $(call INCEPTION_SOURCE_RULE,$(src))))
$(foreach pa,$(PA_STAGE_NAMES),$(eval $(call INCEPTION_STAGE_RULES,$(pa))))
$(foreach checkpoint,$(CHECKPOINTS),$(eval $(call INCEPTION_INCEPTION_RULES,$(checkpoint))))

test-pa37: cppgm++-self

clean:
	-rm -f $(TARGETS) $(INCEPTION_TARGETS)
	-rm -rf $(INCEPTION_OBJ_ROOT_BASE)

.PHONY: all test ladder preservation clean compiler-ready \
	inception compare-inception bitcmp \
	cppgm++-inception-from-restored-self \
	compare-cppgm++-inception-from-restored-self \
	probe-compiler-ready probe-self-object probe-self-link \
	$(TARGETS) $(INCEPTION_TARGETS) $(INCEPTION_COMPARE_TARGETS) \
	$(TEST_TARGETS) $(THROUGH_TARGETS) $(TEST_THROUGH_TARGETS) \
	$(PA_THROUGH_TARGETS) $(PA_TEST_THROUGH_TARGETS) $(PA_TEST_TARGETS) \
	$(PA_TEST_NOBUILD_TARGETS)

-include $(foreach checkpoint,$(CHECKPOINTS),$(call inception_entry_depfile_path,$(checkpoint)))
-include $(foreach src,$(INCEPTION_ALL_SHARED_SOURCES),$(call inception_shared_depfile_path,$(src)))
-include $(inception_runner_depfile_path)

FORCE:
