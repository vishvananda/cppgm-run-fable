#pragma once

#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

using std::map;
using std::ostream;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::vector;

#include "sema/class_info.h"
#include "sema/scope.h"
#include "sema/template_info.h"
#include "sema/type.h"

// The PA12 semantic dump tree: one kind-tagged node per printed line,
// holding the resolved facts (entity name, canonical type, value
// category, operator token) as typed fields. The binder and the
// expression analyzer build the tree in declaration order; the printer
// renders it without consulting the AST or the scope model again.

enum EValueCategory
{
	VC_LVALUE,
	VC_XVALUE,
	VC_PRVALUE
};

const char* ValueCategoryName(EValueCategory category);

enum ESemNodeKind
{
	// declaration items
	SN_TYPE_ALIAS,            // type-alias <name> <type>
	SN_VARIABLE,              // variable <name> <type> [init subtree]
	SN_FUNCTION_DECLARATION,  // function-declaration <name> <type>
	SN_FUNCTION_DEFINITION,   // function-definition <name> <type>
	SN_NAMESPACE_DEFINITION,  // namespace-definition <name>
	SN_PARAMETER,             // parameter <name> <type>
	SN_CONSTRUCTOR_ACTION,    // constructor-action <name>
	// PA15 lifetime action: destroys the object addressed by the call
	// subtree (never printed by the PA12 dump).
	SN_DESTRUCTOR_ACTION,

	// statements
	SN_COMPOUND_STATEMENT,
	SN_SIMPLE_DECLARATION,
	SN_EXPRESSION_STATEMENT,
	SN_RETURN_STATEMENT,
	SN_IF_STATEMENT,
	SN_THEN,
	SN_ELSE,
	SN_WHILE_STATEMENT,
	SN_DO_STATEMENT,
	SN_FOR_STATEMENT,
	SN_FOR_INIT,
	SN_ITERATION,
	SN_CONDITION,
	SN_CONDITION_DECLARATION,
	SN_SWITCH_STATEMENT,
	SN_CASE_STATEMENT,
	SN_DEFAULT_STATEMENT,
	SN_BREAK_STATEMENT,
	SN_CONTINUE_STATEMENT,
	SN_GOTO_STATEMENT,    // PA14: name is the target label
	SN_LABEL_STATEMENT,   // PA14: name is the label, child the statement

	// expressions: <word> <value-category> <type> [extras]
	SN_LITERAL,                // extra: value token
	SN_ID_EXPRESSION,          // extra: name as written
	SN_CALL_EXPRESSION,
	SN_CALLEE,                 // callee <name> <type> (no category)
	SN_UNARY_EXPRESSION,       // extra: TOKEN:spelling
	SN_BINARY_EXPRESSION,      // extra: TOKEN:spelling
	SN_POSTFIX_EXPRESSION,     // extra: TOKEN:spelling
	SN_ASSIGNMENT_EXPRESSION,  // extra: TOKEN:spelling
	SN_CONDITIONAL_EXPRESSION,
	SN_SUBSCRIPT_EXPRESSION,
	SN_MEMBER_EXPRESSION,      // extra: [TOKEN:]member-name
	SN_CAST_EXPRESSION,        // extra: TOKEN:spelling (absent for
	                           // functional casts)
	SN_SIZEOF_EXPRESSION,
	SN_BRACED_INIT_LIST,
	// PA16 synthesized special-member body action: copies `value.bits`
	// bytes (alignment in `bit_width`) from the address of children[1]
	// to the address of children[0]. Lowering-only; never printed by
	// the PA12 dump.
	SN_STORAGE_COPY,
	// PA24 closure construction: one child per closure field, in field
	// order - an lvalue child stores its address (a by-reference
	// capture), a prvalue child stores its pointer value (a captured
	// `this`). The node's type names the closure class.
	SN_CLOSURE_INIT,
	// PA16 scalar new of a non-class type: children[0] is the
	// allocation call, children[1] (optional) the converted
	// initializer value stored into the result. `null_pointer` marks a
	// non-throwing allocation function (construction branches on the
	// result).
	SN_NEW_INIT,
	// PA16 array new: children[0] is the allocation-function callee,
	// children[1] the element-count value, children[2] (optional) the
	// per-element constructor action. `value.bits` is the element
	// size, `member_offset` the count-header bytes (class elements),
	// `trivial_init` requests `()` zero-fill of non-class elements.
	SN_NEW_ARRAY,
	// PA16 delete / delete[]: children[0] is the operand pointer,
	// children[1] the deallocation-function callee, children[2]
	// (optional) the element destructor action. `value.bits` is the
	// element size, `member_offset` the count-header bytes.
	SN_DELETE_EXPRESSION,
	SN_DELETE_ARRAY,
	// PA17 constructor/destructor vpointer store: `type` names the
	// class whose vtable is stored, children[0] the object address.
	// Lowering-only; never printed by the PA12 dump.
	SN_VPOINTER_STORE,
	// PA18 guarded once-only initialization: `name` is the raw LowIR
	// symbol of an i64 guard global; the children run when the guard
	// is still zero, then the guard is set. Lowering-only; never
	// printed by the PA12 dump.
	SN_STATIC_GUARD,
	// PA25 typeid: an lvalue of const std::type_info. `typeid_operand`
	// carries the resolved (cv/reference-stripped) operand type; a
	// dynamic query keeps the polymorphic glvalue as children[0].
	SN_TYPEID
};

// PA15: which ABI entry a function definition / callee names. Complete
// and base entries are identical without virtual bases, so one
// definition serves both; the lowering picks the emitted spelling from
// the demanded variant.
enum ESpecialFunction
{
	SF_NONE,
	SF_CONSTRUCTOR,       // complete-object entry (C1)
	SF_CONSTRUCTOR_BASE,  // base-subobject entry (C2)
	SF_DESTRUCTOR,        // complete-object entry (D1)
	SF_DESTRUCTOR_BASE    // base-subobject entry (D2)
};

struct SemNode;
typedef unique_ptr<SemNode> SemNodePtr;

struct SemNode
{
	explicit SemNode(ESemNodeKind kind_in);

	ESemNodeKind kind;
	string name;          // item / callee / id-expression / member name
	TypePtr type;         // items and expressions (null for statements)
	EValueCategory category;
	bool has_op;          // operator annotation present
	ETokenType op;
	string op_spelling;   // text after the colon
	string token;         // SN_LITERAL value spelling
	vector<SemNodePtr> children;

	// PA14 lowering facts (never printed by the PA12 dump). The named
	// entity's identity is its declaring scope plus declared name; both
	// are stable because scopes are arena-owned by the TypesModel.
	const Scope* entity_scope;
	string entity_name;
	bool has_value;       // decoded constant: literals, enumerator uses,
	                      // sizeof results, folded case values
	ConstValue value;
	// PA20: a folded floating constant (constexpr initializers); the
	// token is the canonical unsuffixed decimal the global-initializer
	// rendering emits.
	bool has_float = false;
	string float_token;
	// PA20 SN_VARIABLE: the init-declarator's terminal token span
	// (begin through the terminating semicolon); the deterministic
	// local-static object names derive from it.
	size_t decl_begin_token = 0;
	size_t decl_end_token = 0;
	bool null_pointer;    // null pointer literal (possibly retyped)
	bool is_string_literal;
	string string_bytes;  // string literal object representation
	// SN_VARIABLE / SN_FUNCTION_DECLARATION / SN_FUNCTION_DEFINITION
	bool is_static_decl;
	bool is_extern_decl;
	bool is_thread_local_decl;
	bool c_linkage;
	bool unwind_no;       // simple noexcept marking on the declarator
	// PA20 SN_CALLEE: the declared (or, for implicit members, the
	// implicit) exception specification is non-throwing; the noexcept
	// operator walk reads this instead of the derived `unwind_no`.
	bool noexcept_decl = false;

	// --- PA15 object-model facts (never printed by the PA12 dump) ---
	// SN_MEMBER_EXPRESSION: resolved layout of the named field. The
	// address is children[0]'s address plus `base_hops` base-subobject
	// projections (all at offset 0) and then `member_offset` bytes.
	unsigned long long member_offset;
	int base_hops;
	bool is_bit_field;
	unsigned long long bit_offset;  // within the unit at member_offset
	unsigned long long bit_width;
	// The field is declared as a reference: uses read through the
	// stored pointer; on an SN_ASSIGNMENT_EXPRESSION the store binds
	// the reference (constructor member initialization).
	bool member_ref;
	// SN_FUNCTION_DEFINITION / SN_CALLEE: non-static member function
	// with the hidden `this` first parameter / argument.
	bool is_method;
	ESpecialFunction special;  // constructor / destructor entry kind
	// PA17 SN_CALLEE: dynamic dispatch through the object's vpointer at
	// this vtable slot (-1: direct call). Explicit qualification and
	// explicit destructor calls stay direct (10.3p15).
	int vtable_slot;
	// SN_FUNCTION_DEFINITION: defined in-class (weak, demand-emitted).
	bool inline_def;
	// SN_FUNCTION_DEFINITION: an out-of-class member spelled `inline`
	// emits weak but unconditionally (the reference prints it unused).
	bool inline_root = false;
	// SN_FUNCTION_DEFINITION: an explicit-specialization definition -
	// emitted only on demand like an instantiation, but with a strong
	// binding (PA23; the reference omits an unused one).
	bool demand_strong = false;
	// PA24 SN_FUNCTION_DEFINITION: a captureless lambda's synthesized
	// function - internal linkage, emitted on demand.
	bool internal_fn = false;
	// SN_CALL_EXPRESSION: the call invokes a user conversion function
	// (12.3.2); object initialization uses the fact to elide an
	// effect-free conversion of an empty class (PA23).
	bool user_conversion = false;
	// SN_FUNCTION_DEFINITION of a conversion function: the bound body
	// performs no observable work (a lone return of an effect-free
	// empty-class temporary), so an empty-object copy-initialization
	// through it lowers to no runtime action (PA23).
	bool conversion_no_work = false;
	// SN_VARIABLE: the object's lifetime ends at scope exit; the
	// attached SN_DESTRUCTOR_ACTION child holds the destruction call.
	bool needs_dtor;
	// SN_VARIABLE: the declaration spelled an initializer. A
	// namespace-scope class object without one anchors the program
	// init helper even when its default construction is trivial (the
	// reference presentation); initialized objects anchor it only
	// through surviving dynamic actions.
	bool has_explicit_init = false;
	// SN_CONSTRUCTOR_ACTION: the implicit default constructor does
	// nothing, so default-initialization emits no call.
	bool trivial_init;
	// PA19 SN_CONSTRUCTOR_ACTION (unaddressed temporaries): 8.5p7
	// value-initialization zero-fills `value.bits` bytes of the target
	// before the (non-user-provided) default constructor runs.
	bool value_zero_fill = false;
	// PA19 SN_LITERAL: lowers as a materialized `const` instruction
	// instead of an immediate (the sizeof... reference shape).
	bool materialize_const = false;
	// A synthesized subobject constructor action whose whole chain does
	// nothing: the callee is still demanded (emitted on use) but the
	// call itself is not printed.
	bool elided;
	// PA15 bit-field stores: the first write to a storage unit inside a
	// constructor stores the masked value directly instead of
	// read-modify-write.
	bool bf_plain_store;

	// --- PA16 value-semantics facts ---
	// SN_CONSTRUCTOR_ACTION: a copy/move-initialization wrapper the
	// binder synthesized around a class-typed source (call arguments,
	// declared-object copy-init, by-value returns). The PA12 dump
	// prints the wrapped source instead of the action.
	bool synth_copy;
	// SN_CONSTRUCTOR_ACTION: the selected copy/move constructor is the
	// trivial implicit one; the lowering emits a raw `copyobj` of the
	// whole object instead of demanding a helper definition.
	bool trivial_copy;
	// PA18: non-empty when this instantiated member body failed to
	// analyze; the definition is ill-formed only if a use demands it
	// (14.7.1), so the lowering throws this message on demand.
	string instantiation_error;
	// PA21 SN_FUNCTION_DEFINITION: the body came from template
	// instantiation, so its references are not definition-time demand
	// (14.7.1: an unused instantiated body is never required).
	bool from_instantiation = false;
	// SN_CALL_EXPRESSION: the call came from overloaded-operator
	// syntax (the checked references drop such calls when they
	// initialize an empty class object).
	bool from_operator = false;
	// SN_CONSTRUCTOR_ACTION: the call's second child is the target
	// object address (declared objects, members, placement results),
	// not the first constructor argument.
	bool ctor_addressed;
	// SN_FUNCTION_DEFINITION: a compiler-synthesized special-member
	// body (implicit/defaulted); emitted only when directly called.
	bool synthesized;
	// PA20 SN_FUNCTION_DEFINITION: declared constexpr (7.1.5); the
	// constant engine only runs constexpr or synthesized bodies.
	bool is_constexpr_fn = false;
	// PA18 SN_FUNCTION_DEFINITION / SN_CALLEE of a function-template
	// specialization: its identity record (the lowering mangles the
	// object name from the pattern signature and argument list).
	const FunctionSpecialization* fn_spec;
	// PA18 SN_VARIABLE: an instantiated static-data-member definition
	// (14.7.1): emits weak with external linkage.
	bool weak_def;
	// SN_CALL_EXPRESSION / SN_CONDITIONAL_EXPRESSION with needs_dtor:
	// the resolved destructor action for the materialized result
	// temporary (12.2), pinned by the binder so the lowering never
	// re-derives the callee. Kept out of `children` so argument
	// lowering and may-throw analysis see only the call's operands.
	SemNodePtr result_dtor;
	// PA25 SN_TYPEID: the resolved operand type of the query, and
	// whether the result reads the operand's dynamic type through its
	// vpointer (children[0] is the polymorphic glvalue then).
	TypePtr typeid_operand;
	bool typeid_dynamic = false;
};

SemNodePtr MakeSemNode(ESemNodeKind kind);

// A deep copy of a node tree (PA15: aggregate-target prototypes and
// the synthesized init/fini helper bodies).
SemNodePtr CloneSemNode(const SemNode& node);

struct ConstObject;

// One translation unit's dump: the declaration-order items plus the
// implicit member functions synthesized while analyzing them (printed
// after the items, in first-need order).
struct SemUnit
{
	vector<SemNodePtr> items;
	vector<SemNodePtr> synthesized;
	// PA20: evaluated constant images of object-valued constexpr
	// definitions, keyed by their SN_VARIABLE item. The lowering emits
	// weak static-member definitions from the image (flattened items)
	// instead of dynamic initialization.
	map<const SemNode*, shared_ptr<const ConstObject>> const_images;
	// The analyzed in-class initialization actions behind an
	// image-backed definition: dropped from emission, but their
	// selected constructors stay odr-used (3.2p3).
	map<const SemNode*, SemNodePtr> const_image_inits;
	// PA15: class metadata and the demand-emitted definitions (in-class
	// methods, hidden friends, constructors/destructors). Deferred
	// definitions are emitted by the lowering only when referenced, in
	// first-demand order.
	ClassRegistry classes;
	vector<SemNodePtr> deferred;
	// PA18: the captured template declarations and their instantiated
	// specializations, plus the class entities named by explicit
	// instantiation definitions (their members emit unconditionally,
	// 14.7.2p8).
	TemplateRegistry templates;
	// 14.7.2p9: only member definitions instantiated by the explicit
	// instantiation itself become emission roots; `deferred_end`
	// bounds the chronological deferred-body list at that point (a
	// member defined later stays an ordinary on-demand member).
	struct ExplicitClassInstantiation
	{
		const NamedTypeInfo* entity = 0;
		size_t deferred_end = 0;
	};
	vector<ExplicitClassInstantiation> explicit_instantiations;
	// PA21 14.7.2 function forms: explicit-instantiation definitions
	// (emitted unconditionally as object roots) and extern-template
	// declarations (declare-only; the strong definition lives in
	// another translation unit).
	vector<const FunctionSpecialization*> explicit_fn_instantiations;
	vector<const FunctionSpecialization*> extern_fn_suppressions;
	// PA21 14.7.2: explicit-instantiation definitions naming a single
	// member of a class-template specialization (the member's
	// instantiated definition emits unconditionally).
	struct ExplicitMemberInstantiation
	{
		const Scope* scope;  // the specialization's member scope
		string name;
	};
	vector<ExplicitMemberInstantiation> explicit_member_instantiations;
};

// Writes the `translation-unit` line and the tree below it.
void PrintSemanticsOutput(const SemUnit& unit, ostream& out);
