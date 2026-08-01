# hosted &&-condition temporary order smoke: the left operand's
# temporary of a statement condition `if (Lock().ok() && use())` must
# survive into the right operand - the whole condition is one full
# expression (12.2). The per-edge cleanup trampolines may only run on
# an edge that leaves the condition, not on the short-circuit
# fall-through into the right operand. Reduced from the PA39
# statement-condition audit; the destructor's flag is read by the
# right operand, so the reference - which defers the destruction -
# and a conforming compiler agree on the output while an early
# destruction flips it.
