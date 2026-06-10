#pragma once

// Character classification shared by translation phase 3 tokenization and
// phase 7 literal analysis. Predicates take a code point.

bool IsDigit(int c);
bool IsOctalDigit(int c);
bool IsHexDigit(int c);

// Value of a hexadecimal digit, or -1 if c is not one.
int HexDigitValue(int c);

// 2.11: a-z, A-Z, underscore.
bool IsNondigit(int c);

// 2.11 Identifiers: nondigit or an Annex E.1 code point that Annex E.2
// does not disallow initially / allows in continuation position.
bool IsIdentifierStart(int c);
bool IsIdentifierContinue(int c);

// 2.13.3 Table 7: characters that may follow the backslash of a
// simple-escape-sequence.
bool IsSimpleEscapeChar(int c);
