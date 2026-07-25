# hosted string conditional-copy smoke: a glvalue arm of a class-typed
# conditional copy-initializes the materialized result (a raw in-place
# arm copy would alias the source string's heap buffer and double
# free).
