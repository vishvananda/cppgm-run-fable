#include "pp_token.h"

#include <algorithm>
#include <iterator>
#include <utility>

bool PaintContains(const PaintSet& paint, const string& name)
{
	return paint && std::binary_search(paint->begin(), paint->end(), name);
}

PaintSet PaintInterner::Intern(vector<string>& names)
{
	map<vector<string>, PaintSet>::iterator it = sets_.find(names);
	if (it == sets_.end())
	{
		PaintSet made(new vector<string>(names));
		it = sets_.insert(std::make_pair(names, made)).first;
	}
	return it->second;
}

PaintSet PaintInterner::Insert(const PaintSet& paint, const string& name)
{
	if (PaintContains(paint, name))
		return paint;
	pair<PaintKey, string> key(paint.get(), name);
	map<pair<PaintKey, string>, PaintSet>::iterator memo =
		insert_memo_.find(key);
	if (memo != insert_memo_.end())
		return memo->second;
	vector<string> names;
	if (paint)
		names = *paint;
	names.insert(std::lower_bound(names.begin(), names.end(), name), name);
	PaintSet result = Intern(names);
	insert_memo_[key] = result;
	return result;
}

PaintSet PaintInterner::Union(const PaintSet& a, const PaintSet& b)
{
	if (a == b || !b)
		return a;
	if (!a)
		return b;
	pair<PaintKey, PaintKey> key(std::min(a.get(), b.get()),
	                             std::max(a.get(), b.get()));
	map<pair<PaintKey, PaintKey>, PaintSet>::iterator memo =
		union_memo_.find(key);
	if (memo != union_memo_.end())
		return memo->second;
	vector<string> names;
	std::set_union(a->begin(), a->end(), b->begin(), b->end(),
	               std::back_inserter(names));
	PaintSet result;
	if (names.size() == a->size())
		result = a;
	else if (names.size() == b->size())
		result = b;
	else
		result = Intern(names);
	union_memo_[key] = result;
	return result;
}

PaintSet PaintInterner::Intersect(const PaintSet& a, const PaintSet& b)
{
	if (a == b)
		return a;
	if (!a || !b)
		return PaintSet();
	pair<PaintKey, PaintKey> key(std::min(a.get(), b.get()),
	                             std::max(a.get(), b.get()));
	map<pair<PaintKey, PaintKey>, PaintSet>::iterator memo =
		intersect_memo_.find(key);
	if (memo != intersect_memo_.end())
		return memo->second;
	vector<string> names;
	std::set_intersection(a->begin(), a->end(), b->begin(), b->end(),
	                      std::back_inserter(names));
	PaintSet result;
	if (names.size() == a->size())
		result = a;
	else if (names.size() == b->size())
		result = b;
	else if (!names.empty())
		result = Intern(names);
	intersect_memo_[key] = result;
	return result;
}

void PPTokenCollector::token_start_line(long line)
{
	pending_line_ = line;
}

void PPTokenCollector::Add(EPPTokenKind kind, const string& data)
{
	PPToken token(kind, data);
	token.ws_before = pending_ws_;
	token.line = pending_line_;
	pending_ws_ = false;
	tokens.push_back(token);
}

void PPTokenCollector::emit_whitespace_sequence()
{
	pending_ws_ = true;
}

void PPTokenCollector::emit_new_line()
{
	Add(PPT_NEW_LINE, "\n");
}

void PPTokenCollector::emit_header_name(const string& data)
{
	Add(PPT_HEADER_NAME, data);
}

void PPTokenCollector::emit_identifier(const string& data)
{
	Add(PPT_IDENTIFIER, data);
}

void PPTokenCollector::emit_pp_number(const string& data)
{
	Add(PPT_PP_NUMBER, data);
}

void PPTokenCollector::emit_character_literal(const string& data)
{
	Add(PPT_CHARACTER_LITERAL, data);
}

void PPTokenCollector::emit_user_defined_character_literal(const string& data)
{
	Add(PPT_UD_CHARACTER_LITERAL, data);
}

void PPTokenCollector::emit_string_literal(const string& data)
{
	Add(PPT_STRING_LITERAL, data);
}

void PPTokenCollector::emit_user_defined_string_literal(const string& data)
{
	Add(PPT_UD_STRING_LITERAL, data);
}

void PPTokenCollector::emit_preprocessing_op_or_punc(const string& data)
{
	Add(PPT_OP_OR_PUNC, data);
}

void PPTokenCollector::emit_non_whitespace_char(const string& data)
{
	Add(PPT_NON_WHITESPACE_CHAR, data);
}

void PPTokenCollector::emit_eof()
{
	pending_ws_ = false;
}

void EmitPPToken(const PPToken& token, IPPTokenStream& output)
{
	switch (token.kind)
	{
	case PPT_NEW_LINE:
		output.emit_new_line();
		break;
	case PPT_HEADER_NAME:
		output.emit_header_name(token.data);
		break;
	case PPT_IDENTIFIER:
		output.emit_identifier(token.data);
		break;
	case PPT_PP_NUMBER:
		output.emit_pp_number(token.data);
		break;
	case PPT_CHARACTER_LITERAL:
		output.emit_character_literal(token.data);
		break;
	case PPT_UD_CHARACTER_LITERAL:
		output.emit_user_defined_character_literal(token.data);
		break;
	case PPT_STRING_LITERAL:
		output.emit_string_literal(token.data);
		break;
	case PPT_UD_STRING_LITERAL:
		output.emit_user_defined_string_literal(token.data);
		break;
	case PPT_OP_OR_PUNC:
		output.emit_preprocessing_op_or_punc(token.data);
		break;
	case PPT_NON_WHITESPACE_CHAR:
		output.emit_non_whitespace_char(token.data);
		break;
	case PPT_PLACEMARKER:
		break;
	}
}

bool IsHash(const PPToken& token)
{
	return token.kind == PPT_OP_OR_PUNC &&
		(token.data == "#" || token.data == "%:");
}

bool IsHashHash(const PPToken& token)
{
	return token.kind == PPT_OP_OR_PUNC &&
		(token.data == "##" || token.data == "%:%:");
}

bool IsOp(const PPToken& token, const char* spelling)
{
	return token.kind == PPT_OP_OR_PUNC && token.data == spelling;
}
