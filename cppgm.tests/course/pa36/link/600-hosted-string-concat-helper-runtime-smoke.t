# hosted string-concat smoke: operator+ calls std::__str_concat<_Str>
# with a local alias as the explicit argument; its body contains the
# vexing-parse shape `_Str __str(_Alloc_traits::_S_select_on_copy(__a))`
# which must re-read as a variable with a parenthesized initializer.
