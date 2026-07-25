# hosted secondary-vtable smoke: a two-interface class whose key
# function lives in another TU; the using TU's inline constructor
# stores the secondary view vpointer, whose weak vtable must render in
# every emitting unit even when the primary is declare-only.
