#pragma once

#include <string>

using std::string;

#include "IPPTokenStream.h"
#include "source_translation.h"

// Translation phase 3: decomposes the translated source into preprocessing
// tokens and emits them to output, ending with eof. Throws
// std::runtime_error for ill-formed input (unterminated comments, literals,
// raw string delimiters, invalid escape sequences). When `lines` is given,
// each token emission is preceded by the token's physical start line.
void TokenizePPTokens(const TranslatedSource& source, IPPTokenStream& output,
                      IPPTokenLineSink* lines = 0);
