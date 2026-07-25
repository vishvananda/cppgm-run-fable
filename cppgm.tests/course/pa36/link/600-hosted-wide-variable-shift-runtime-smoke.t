# hosted variable-count __int128 shift smoke: shl, logical shr, and
# arithmetic shr by runtime counts 0..127 print their full results,
# compared against the reference (which lowers these through libgcc's
# __ashlti3 family; the whole-program pa28 harness cannot link those,
# so the hosted suite is the earliest harness that can express the
# behavior). Reduced from dev/src/sema/const_expr.cpp's
# `wide >> count` const-eval helpers, which the PA39 self-host build
# compiles.
