#pragma once

#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

using std::map;
using std::ostream;
using std::pair;
using std::string;
using std::unique_ptr;
using std::vector;

#include "post_token.h"
#include "sema/entity.h"

// PA8 program model: the cross-translation-unit half of the semantic
// state. One Program owns every DeclaredEntity, lifetime-extended
// temporary, and string-literal object of the program; entities with
// external linkage are linked by key (home namespace path + name, plus
// the parameter signature for functions, 3.5), so a later translation
// unit's declaration returns the already-created entity. nsdecl builds
// a throwaway Program per translation unit, which makes linking a
// no-op there and keeps PA7 behaviour unchanged.
//
// The image block lists are recorded in creation order during the
// single forward parse, which is exactly the order the PA8 handout
// requires: Block 1 by first declaration within the program, Block 2
// (temporaries) by first use, Block 3 (string literals) by token order.
class Program
{
public:
	Program();

	int current_tu() const
	{
		return tu_;
	}

	// Marks the start of the next translation unit (first call: index 0).
	void BeginTranslationUnit();

	// The program-wide identity of the namespace `name` declared in the
	// namespace with identity `parent_id` (global namespace: 0). The
	// same path from any translation unit yields the same id, giving
	// linking keys an O(1) namespace component.
	int InternNamespace(int parent_id, const string& name);

	// Returns the entity for a first-in-this-TU declaration: for an
	// external `key`, the entity another translation unit already
	// declared under that key if any, otherwise a new entity (which is
	// appended to Block 1). Internal-linkage declarations pass an empty
	// key and always create. `created` reports which happened; the
	// caller merges types and flags into an existing entity.
	DeclaredEntity* LinkEntity(const string& key, ESlotKind kind,
	                           const string& name, const TypePtr& type,
	                           ELinkage linkage, bool& created);

	// Records a definition and enforces the one-definition rule (3.2):
	// variables define once per program; functions once, except inline
	// functions which define at most once per translation unit.
	void RecordDefinition(DeclaredEntity* entity);

	// A lifetime-extended temporary bound to a reference (Block 2).
	ImageSlot* CreateTemporary(const TypePtr& type);

	// The object for one string-literal token (Block 3): type is array
	// of N const element, value bytes are the token's, including the
	// terminator.
	ImageSlot* CreateStringLiteral(const PostToken& token);

	// Lays out the image (assigning slot offsets) and writes it: magic,
	// Block 1 (skipping never-defined variables), Block 2, Block 3,
	// with zero padding to each object's alignment and relocations
	// resolved (unplaced targets resolve to offset 0).
	void WriteImage(ostream& out);

private:
	vector<unique_ptr<DeclaredEntity>> entities_;
	vector<unique_ptr<ImageSlot>> objects_;  // temporaries and strings
	map<pair<int, string>, int> namespace_ids_;  // (parent id, name) -> id
	map<string, DeclaredEntity*> linked_;    // external key -> entity
	vector<DeclaredEntity*> block1_;
	vector<ImageSlot*> block2_;
	vector<ImageSlot*> block3_;
	int tu_;
};
