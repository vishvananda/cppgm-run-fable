# hosted if-condition declaration scope smoke: an if-condition
# variable (6.4p3) lives exactly to the end of its selection
# statement. A braceless `if (k) if (Ptr p = make()) return p;`
# nesting must not leak p's destructor into the enclosing scope -
# exits there would destroy an object the false path never
# constructed (reduced from AstParser::ParseUnaryExpression, whose
# self-built form freed a garbage pointer on every non-identifier
# token). Prints the selected values for all three paths.
