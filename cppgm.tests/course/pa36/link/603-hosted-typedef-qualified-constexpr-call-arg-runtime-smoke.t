# hosted typedef-qualified constexpr-call template-argument smoke:
# `bool_c<node_traits::nothrow_move()>()` spells a zero-argument
# constexpr static member call behind a typedef-spelled qualifier as
# an alias-template value argument. The argument parses as a function
# type-id, and the value re-read must evaluate the resolved ordinary
# (non-template) member through the full constant engine over its
# analyzed definition. Reduced from stl_tree.h's
# `__bool_constant<_Node_alloc_traits::_S_nothrow_move()>()` in
# _Rb_tree::operator=, which the PA39 self-host build compiles in
# dev/src/sema/sem_spec.cpp and sem_template.cpp.
