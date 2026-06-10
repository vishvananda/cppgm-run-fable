#pragma once

#include <ostream>
#include <vector>

#include "ast/ast.h"

// Deterministic PA10 dump: writes the `<n> translation units` header
// and one `start translation unit <k>` ... `end translation unit`
// block per parsed unit, in the exact node vocabulary, ordering, and
// indentation the checked-in references gate.
void PrintAstOutput(const std::vector<AstDeclPtr>& units, std::ostream& out);
