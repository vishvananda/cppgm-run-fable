# hosted conditional-expression temporary lifetime smoke: a class
# temporary created earlier in a full expression must survive a
# conditional-expression (or short-circuit) branch inside that same
# expression - 12.2 destroys it at the end of the full expression,
# not on the branch edges (those trampolines belong to statement
# conditions only). Reduced from ast_printer.cpp's
# `string(definition ? "..." : "...") + (name ? Flatten(...) :
# string())`, where the self-built compiler freed the left operand's
# heap buffer before operator+ appended into it.
