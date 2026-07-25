# hosted aggregate-array-member smoke: an array of aggregates whose
# elements carry an array member initialized from a braced sub-list
# (the parse_expr.cpp kBinaryLevels shape). The synthesized aggregate
# constructor takes the array member as a decayed pointer to a
# caller-materialized argument array and raw-copies the span; omitted
# array members and scalar tails zero-fill. Covers namespace-scope
# (dynamic init) and local arrays, full and partial element lists.
