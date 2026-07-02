# Per-tool implementation source lists for the compiler.
#
# Add dev/src/foo.cpp to the tools that use it by adding `foo` below. For
# subdirectories, use the path without `.cpp`, such as `parser/foo`.

FRONTEND_SOURCE_SET_TARGETS := abimangle pptoken posttoken ctrlexpr macro preproc recog nsdecl nsinit cy86 cppgm++ lowiropt lowir2cy86 lowir2native

FRONTEND_OBJ_BASENAMES_abimangle :=
FRONTEND_OBJ_BASENAMES_pptoken := pp_tokenizer source_translation utf8 lex_char_classes
FRONTEND_OBJ_BASENAMES_posttoken := pp_tokenizer source_translation utf8 lex_char_classes post_token post_tokenizer numeric_literals text_literals
FRONTEND_OBJ_BASENAMES_ctrlexpr := pp_tokenizer source_translation utf8 lex_char_classes post_token numeric_literals text_literals ctrl_expr
FRONTEND_OBJ_BASENAMES_macro := pp_tokenizer source_translation utf8 lex_char_classes post_token post_tokenizer numeric_literals text_literals pp_token macro_table macro_expand macro_preprocess
FRONTEND_OBJ_BASENAMES_preproc := pp_tokenizer source_translation utf8 lex_char_classes post_token post_tokenizer numeric_literals text_literals pp_token macro_table macro_expand ctrl_expr preprocess predefined_macros
FRONTEND_OBJ_BASENAMES_recog := pp_tokenizer source_translation utf8 lex_char_classes post_token post_tokenizer numeric_literals text_literals pp_token macro_table macro_expand ctrl_expr preprocess predefined_macros parse/parse_token parse/parse_tree parse/parser_core parse/parse_names parse/parse_expr parse/parse_stmt parse/parse_decl parse/parse_declarator parse/parse_class
FRONTEND_OBJ_BASENAMES_nsdecl := pp_tokenizer source_translation utf8 lex_char_classes post_token post_tokenizer numeric_literals text_literals pp_token macro_table macro_expand ctrl_expr preprocess predefined_macros sema/type sema/entity sema/name_lookup sema/expr sema/init sema/program sema/decl_parser
FRONTEND_OBJ_BASENAMES_nsinit := $(FRONTEND_OBJ_BASENAMES_nsdecl)
FRONTEND_OBJ_BASENAMES_cy86 := pp_tokenizer source_translation utf8 lex_char_classes post_token post_tokenizer numeric_literals text_literals pp_token macro_table macro_expand ctrl_expr preprocess predefined_macros cy86/cy86_parser cy86/cy86_codegen x86/x86_encoding x86/elf_program
FRONTEND_OBJ_BASENAMES_cppgm++ := pp_tokenizer source_translation utf8 lex_char_classes post_token post_tokenizer numeric_literals text_literals pp_token macro_table macro_expand ctrl_expr preprocess predefined_macros parse/parse_token ast/ast ast/ast_text ast/ast_printer ast/ast_parser_core ast/ast_parse_names ast/ast_parse_expr ast/ast_parse_stmt ast/ast_parse_decl ast/ast_parse_declarator ast/ast_parse_class sema/type sema/scope sema/scope_lookup sema/type_builder sema/const_expr sema/class_info sema/decl_binder sema/decl_function sema/template_info sema/template_args sema/sem_pack sema/sem_template sema/template_deduce sema/sem_template_check sema/sem_node sema/sem_convert sema/sem_expr sema/sem_cast sema/sem_call sema/sem_binder sema/sem_class sema/sem_member_body sema/sem_special sema/sem_new sema/sem_ctor sema/sem_virtual sema/sem_lifetime sema/sem_member sema/sem_operator lowering/lower_types lowering/lower_name lowering/lower_const lowering/lower_unit lowering/lower_function lowering/lower_expr lowering/lower_convert lowering/lower_member lowering/lower_new lowering/lower_vtable
FRONTEND_OBJ_BASENAMES_lowiropt :=
FRONTEND_OBJ_BASENAMES_lowir2cy86 := lowir/lowir_program lowir/lowir_lexer lowir/lowir_parser lowir/lowir_validate lowir/lowir_frame lowir/lowir_emit_values lowir/lowir_emit_inst lowir/lowir_emit_program
FRONTEND_OBJ_BASENAMES_lowir2native :=
