# hosted string move-assign smoke: __alloc_on_move odr-uses the
# defaulted allocator<char>::operator= of the extern-instantiated
# allocator<char>; the synthesized member must stay locally emitted
# (libstdc++ never materializes it).
