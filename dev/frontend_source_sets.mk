# Per-tool implementation source lists for the compiler.
#
# Add dev/src/foo.cpp to the tools that use it by adding `foo` below. For
# subdirectories, use the path without `.cpp`, such as `parser/foo`.

FRONTEND_SOURCE_SET_TARGETS := abimangle pptoken posttoken ctrlexpr macro preproc recog nsdecl nsinit cy86 cppgm++ lowiropt lowir2cy86 lowir2native

FRONTEND_OBJ_BASENAMES_abimangle :=
FRONTEND_OBJ_BASENAMES_pptoken := pp_tokenizer source_translation utf8
FRONTEND_OBJ_BASENAMES_posttoken :=
FRONTEND_OBJ_BASENAMES_ctrlexpr :=
FRONTEND_OBJ_BASENAMES_macro :=
FRONTEND_OBJ_BASENAMES_preproc :=
FRONTEND_OBJ_BASENAMES_recog :=
FRONTEND_OBJ_BASENAMES_nsdecl :=
FRONTEND_OBJ_BASENAMES_nsinit :=
FRONTEND_OBJ_BASENAMES_cy86 :=
FRONTEND_OBJ_BASENAMES_cppgm++ :=
FRONTEND_OBJ_BASENAMES_lowiropt :=
FRONTEND_OBJ_BASENAMES_lowir2cy86 :=
FRONTEND_OBJ_BASENAMES_lowir2native :=
