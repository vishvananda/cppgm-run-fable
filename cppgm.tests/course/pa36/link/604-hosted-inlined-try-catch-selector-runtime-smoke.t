# hosted inlined-try-catch selector smoke: a small function whose
# body is one try/catch is called outside any protected region by a
# function with its own try/catch of a different type. The PA37
# inliner must not paste an EH-bearing callee: its eh_catch selectors
# are function-local ids also baked into the pad's dispatch compares
# as plain literals, and the caller's LSDA requires per-function
# -unique filter/type pairs - the merged body collides on selector 1
# ("eh filter maps to conflicting catch types" at object emission).
# Reduced from dev/src/sema/template_spelling.cpp's SpellBail helpers
# in the PA39 self-host build.
