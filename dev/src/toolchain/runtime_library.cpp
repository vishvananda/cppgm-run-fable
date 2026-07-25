#include "toolchain/runtime_library.h"

#include <stdexcept>
#include <vector>

// The runtime source is C++ in the compiler's own supported subset.
// Layout contracts (all fixed by the Itanium ABI or by this
// implementation's backend):
//
// - Exception records: {dtor, type_info, next_caught, handler_count,
//   rethrown, slot, pad}, payload at +64, allocated from a static
//   slot arena (no host allocator exists at this stage).
// - RTTI records: the first word points 16 bytes past one of the
//   __cxxabiv1 vtable anchor objects defined here; the anchor address
//   classifies the record kind. Class records are {vptr, name},
//   single-inheritance adds the base record, vmi adds
//   {u32 flags, u32 count, {base, i64 offset_flags}...} where
//   offset_flags is (offset << 8 | flags) and flag bit 2 marks a
//   public base.
// - Vtables: the address point is preceded by offset-to-top (-16)
//   and the RTTI pointer (-8), which __dynamic_cast reads.
// - The linker's support module owns the __cppgm_get_frame /
//   __cppgm_land / __cppgm_exit primitives and the
//   __cppgm_eh_region_table pointer slot; the linker synthesizes the
//   region table itself from the modules' typed host-EH facts.

namespace toolchain {

namespace {

const char * const kRuntimeLines[] = {
	"typedef unsigned long cppgm_usize;",
	"typedef void (*cppgm_dtor)(void *);",
	"",
	"extern \"C\" { extern unsigned long * __cppgm_eh_region_table; }",
	"extern \"C\" unsigned long __cppgm_get_frame();",
	"extern \"C\" void __cppgm_land(unsigned long lp, unsigned long rbp,",
	"                              void * cookie, long selector);",
	"extern \"C\" void __cppgm_exit(long status);",
	"",
	"extern \"C\" unsigned long",
	"    _ZTVN10__cxxabiv117__class_type_infoE[8] = {0};",
	"extern \"C\" unsigned long",
	"    _ZTVN10__cxxabiv120__si_class_type_infoE[8] = {0};",
	"extern \"C\" unsigned long",
	"    _ZTVN10__cxxabiv121__vmi_class_type_infoE[8] = {0};",
	"extern \"C\" unsigned long",
	"    _ZTVN10__cxxabiv119__pointer_type_infoE[8] = {0};",
	"extern \"C\" unsigned long",
	"    _ZTVN10__cxxabiv123__fundamental_type_infoE[8] = {0};",
	"",
	"namespace {",
	"",
	"struct CppgmEhRecord",
	"{",
	"  cppgm_dtor dtor;",
	"  void * tinfo;",
	"  CppgmEhRecord * next_caught;",
	"  long handler_count;",
	"  long rethrown;",
	"  long slot;",
	"  void * adjusted;",
	"  long pad;",
	"};",
	"",
	"enum { CPPGM_EH_SLOTS = 16, CPPGM_EH_SLOT_WORDS = 512 };",
	"",
	"unsigned long cppgm_eh_arena[CPPGM_EH_SLOTS][CPPGM_EH_SLOT_WORDS];",
	"long cppgm_eh_arena_used[CPPGM_EH_SLOTS];",
	"CppgmEhRecord * cppgm_eh_caught_top;",
	"",
	"char * cppgm_eh_payload(CppgmEhRecord * record)",
	"{",
	"  return (char *)record + sizeof(CppgmEhRecord);",
	"}",
	"",
	"CppgmEhRecord * cppgm_eh_record_of(void * payload)",
	"{",
	"  return (CppgmEhRecord *)((char *)payload - sizeof(CppgmEhRecord));",
	"}",
	"",
	"void * cppgm_anchor_point(unsigned long * anchor)",
	"{",
	"  return (char *)anchor + 16;",
	"}",
	"",
	"// The record-kind anchors: a type_info record's first word.",
	"void * cppgm_record_kind(void * record)",
	"{",
	"  return *(void **)record;",
	"}",
	"",
	"bool cppgm_is_class_kind(void * kind)",
	"{",
	"  return kind == cppgm_anchor_point(_ZTVN10__cxxabiv117__class_type_infoE)",
	"      || kind == cppgm_anchor_point(_ZTVN10__cxxabiv120__si_class_type_infoE)",
	"      || kind == cppgm_anchor_point(_ZTVN10__cxxabiv121__vmi_class_type_infoE);",
	"}",
	"",
	"// Walks `record`'s class hierarchy looking for `target` reachable",
	"// through public paths, accumulating the subobject offset.",
	"bool cppgm_find_public_base(void * record, void * target,",
	"                            long offset, long * found_offset)",
	"{",
	"  if (record == target)",
	"  {",
	"    *found_offset = offset;",
	"    return true;",
	"  }",
	"  void * kind = cppgm_record_kind(record);",
	"  if (kind ==",
	"      cppgm_anchor_point(_ZTVN10__cxxabiv120__si_class_type_infoE))",
	"  {",
	"    void * base = *(void **)((char *)record + 16);",
	"    return cppgm_find_public_base(base, target, offset, found_offset);",
	"  }",
	"  if (kind ==",
	"      cppgm_anchor_point(_ZTVN10__cxxabiv121__vmi_class_type_infoE))",
	"  {",
	"    unsigned int count = *(unsigned int *)((char *)record + 20);",
	"    for (unsigned int i = 0; i < count; i++)",
	"    {",
	"      char * entry = (char *)record + 24 + 16 * (long)i;",
	"      void * base = *(void **)entry;",
	"      long offset_flags = *(long *)(entry + 8);",
	"      if ((offset_flags & 2) == 0)",
	"        continue;",
	"      if (cppgm_find_public_base(base, target,",
	"                                 offset + (offset_flags >> 8),",
	"                                 found_offset))",
	"        return true;",
	"    }",
	"  }",
	"  return false;",
	"}",
	"",
	"// 15.3: does a handler of type `catch_ti` match an exception whose",
	"// static type record is `thrown_ti`? On success `*adjusted` is the",
	"// object pointer the handler receives.",
	"bool cppgm_eh_type_matches(void * catch_ti, void * thrown_ti,",
	"                           char * payload, void ** adjusted)",
	"{",
	"  if (catch_ti == thrown_ti)",
	"  {",
	"    *adjusted = payload;",
	"    return true;",
	"  }",
	"  if (cppgm_is_class_kind(cppgm_record_kind(thrown_ti)) &&",
	"      cppgm_is_class_kind(cppgm_record_kind(catch_ti)))",
	"  {",
	"    long offset = 0;",
	"    if (cppgm_find_public_base(thrown_ti, catch_ti, 0, &offset))",
	"    {",
	"      *adjusted = payload + offset;",
	"      return true;",
	"    }",
	"    return false;",
	"  }",
	"  if (cppgm_record_kind(thrown_ti) ==",
	"          cppgm_anchor_point(_ZTVN10__cxxabiv119__pointer_type_infoE) &&",
	"      cppgm_record_kind(catch_ti) ==",
	"          cppgm_anchor_point(_ZTVN10__cxxabiv119__pointer_type_infoE))",
	"  {",
	"    void * thrown_pointee = *(void **)((char *)thrown_ti + 24);",
	"    void * catch_pointee = *(void **)((char *)catch_ti + 24);",
	"    if (thrown_pointee == catch_pointee)",
	"    {",
	"      *adjusted = payload;",
	"      return true;",
	"    }",
	"    long offset = 0;",
	"    if (cppgm_is_class_kind(cppgm_record_kind(thrown_pointee)) &&",
	"        cppgm_is_class_kind(cppgm_record_kind(catch_pointee)) &&",
	"        cppgm_find_public_base(thrown_pointee, catch_pointee, 0,",
	"                               &offset))",
	"    {",
	"      *(char **)payload += offset;",
	"      *adjusted = payload;",
	"      return true;",
	"    }",
	"  }",
	"  return false;",
	"}",
	"",
	"// One matched region row: land at its pad when an action applies.",
	"// Records are {kind, filter, type_info}: 1 typed catch, 2 catch-",
	"// all, 3 cleanup, 0 end. A null record list is a pure cleanup row.",
	"// Landing installs the Itanium contract (cookie in rax, selector",
	"// in edx) and never returns.",
	"void cppgm_eh_dispatch_row(unsigned long * row, unsigned long rbp,",
	"                           void * cookie, CppgmEhRecord * record)",
	"{",
	"  unsigned long lp = row[2];",
	"  unsigned long * action = (unsigned long *)row[3];",
	"  if (action == 0)",
	"    __cppgm_land(lp, rbp, cookie, 0);",
	"  bool cleanup = false;",
	"  while (action[0] != 0)",
	"  {",
	"    if (action[0] == 3)",
	"      cleanup = true;",
	"    else if (action[0] == 2)",
	"      __cppgm_land(lp, rbp, cookie, (long)action[1]);",
	"    else if (record != 0)",
	"    {",
	"      void * adjusted = cppgm_eh_payload(record);",
	"      if (cppgm_eh_type_matches((void *)action[2], record->tinfo,",
	"                                cppgm_eh_payload(record), &adjusted))",
	"      {",
	"        record->adjusted = adjusted;",
	"        __cppgm_land(lp, rbp, cookie, (long)action[1]);",
	"      }",
	"    }",
	"    action = action + 3;",
	"  }",
	"  if (cleanup)",
	"    __cppgm_land(lp, rbp, cookie, 0);",
	"}",
	"",
	"// The private unwinder: walk the rbp frame chain from `fp` and",
	"// dispatch the first region row covering each frame's call site.",
	"// The linker synthesizes the region table from the same typed",
	"// host-EH facts object emission renders into .eh_frame/LSDA data;",
	"// generated frames are uniformly rbp-based, so no CFI is needed.",
	"// The chain ends at the start sentinel: unhandled exits 134.",
	"void cppgm_eh_raise_from(unsigned long fp, void * cookie,",
	"                         CppgmEhRecord * record)",
	"{",
	"  unsigned long * table = __cppgm_eh_region_table;",
	"  unsigned long count = table == 0 ? 0 : table[0];",
	"  while (fp != 0)",
	"  {",
	"    unsigned long ra = ((unsigned long *)fp)[1];",
	"    unsigned long caller_rbp = ((unsigned long *)fp)[0];",
	"    for (unsigned long i = 0; i < count; i++)",
	"    {",
	"      unsigned long * row = table + 1 + 4 * i;",
	"      if (ra - 1 >= row[0] && ra - 1 < row[1])",
	"        cppgm_eh_dispatch_row(row, caller_rbp, cookie, record);",
	"    }",
	"    fp = caller_rbp;",
	"  }",
	"  __cppgm_exit(134);",
	"}",
	"",
	"}  // namespace",
	"",
	"extern \"C\" void * __cxa_allocate_exception(cppgm_usize size)",
	"{",
	"  cppgm_usize need = size + sizeof(CppgmEhRecord);",
	"  if (need > sizeof(cppgm_eh_arena[0]))",
	"    __cppgm_exit(134);",
	"  for (long i = 0; i < CPPGM_EH_SLOTS; i++)",
	"  {",
	"    if (cppgm_eh_arena_used[i])",
	"      continue;",
	"    cppgm_eh_arena_used[i] = 1;",
	"    CppgmEhRecord * record = (CppgmEhRecord *)cppgm_eh_arena[i];",
	"    record->dtor = 0;",
	"    record->tinfo = 0;",
	"    record->next_caught = 0;",
	"    record->handler_count = 0;",
	"    record->rethrown = 0;",
	"    record->slot = i;",
	"    record->adjusted = 0;",
	"    return cppgm_eh_payload(record);",
	"  }",
	"  __cppgm_exit(134);",
	"  return 0;",
	"}",
	"",
	"extern \"C\" void __cxa_free_exception(void * payload)",
	"{",
	"  CppgmEhRecord * record = cppgm_eh_record_of(payload);",
	"  cppgm_eh_arena_used[record->slot] = 0;",
	"}",
	"",
	"extern \"C\" void __cxa_throw(void * payload, void * tinfo,",
	"                             cppgm_dtor dtor)",
	"{",
	"  CppgmEhRecord * record = cppgm_eh_record_of(payload);",
	"  record->tinfo = tinfo;",
	"  record->dtor = dtor;",
	"  record->adjusted = payload;",
	"  cppgm_eh_raise_from(__cppgm_get_frame(), record, record);",
	"}",
	"",
	"// The PA13 plain-throw form: the raw payload value is the cookie;",
	"// only catch-all and cleanup actions can apply (no type_info).",
	"extern \"C\" void __cppgm_throw_value(void * value)",
	"{",
	"  cppgm_eh_raise_from(__cppgm_get_frame(), value, 0);",
	"}",
	"",
	"extern \"C\" void * __cxa_begin_catch(void * cookie)",
	"{",
	"  CppgmEhRecord * record = (CppgmEhRecord *)cookie;",
	"  record->handler_count = record->handler_count + 1;",
	"  if (record != cppgm_eh_caught_top)",
	"  {",
	"    record->next_caught = cppgm_eh_caught_top;",
	"    cppgm_eh_caught_top = record;",
	"  }",
	"  return record->adjusted;",
	"}",
	"",
	"extern \"C\" void __cxa_end_catch()",
	"{",
	"  CppgmEhRecord * record = cppgm_eh_caught_top;",
	"  if (record == 0)",
	"    __cppgm_exit(134);",
	"  record->handler_count = record->handler_count - 1;",
	"  if (record->handler_count > 0)",
	"    return;",
	"  cppgm_eh_caught_top = record->next_caught;",
	"  if (record->rethrown)",
	"  {",
	"    record->rethrown = 0;",
	"    return;",
	"  }",
	"  if (record->dtor)",
	"    record->dtor(cppgm_eh_payload(record));",
	"  cppgm_eh_arena_used[record->slot] = 0;",
	"}",
	"",
	"extern \"C\" void __cxa_rethrow()",
	"{",
	"  CppgmEhRecord * record = cppgm_eh_caught_top;",
	"  if (record == 0)",
	"    __cppgm_exit(134);",
	"  record->rethrown = 1;",
	"  cppgm_eh_raise_from(__cppgm_get_frame(), record, record);",
	"}",
	"",
	"// Cleanup landing pads resume with the cookie they landed with;",
	"// the walk continues at the resuming frame's own call-site row",
	"// (outer regions only - the landed region is statically gone).",
	"extern \"C\" void _Unwind_Resume(void * cookie)",
	"{",
	"  cppgm_eh_raise_from(__cppgm_get_frame(), cookie,",
	"                      (CppgmEhRecord *)cookie);",
	"}",
	"",
	"// Host-form objects carry a DW.ref-style personality slot naming",
	"// the host C++ personality. The private unwinder dispatches from",
	"// its own tables and never calls through the slot, but the slot's",
	"// address patch must still resolve; reaching this body means a",
	"// foreign unwinder ran, which the freestanding runtime cannot",
	"// support.",
	"extern \"C\" int __gxx_personality_v0()",
	"{",
	"  __cppgm_exit(134);",
	"  return 0;",
	"}",
	"",
	"// PA33 15.4p9: an exception escaping a noexcept function lands in",
	"// the terminate trampoline, which ends here. The freestanding",
	"// default handler ends the program with the abort status.",
	"namespace std {",
	"void terminate()",
	"{",
	"  __cppgm_exit(134);",
	"}",
	"}",
	"",
	"// __dynamic_cast helpers: subobject searches over the RTTI graph.",
	"namespace {",
	"",
	"struct CppgmCastSearch",
	"{",
	"  void * dst_ti;",
	"  void * src_ti;",
	"  char * src_address;",
	"  char * result;",
	"  long result_count;",
	"  long src_public_in_whole;",
	"};",
	"",
	"// Enumerates every subobject; records public dst subobjects and",
	"// whether the src subobject sits on a public path from the root.",
	"void cppgm_cast_walk(CppgmCastSearch * search, void * record,",
	"                     char * address, bool is_public)",
	"{",
	"  if (record == search->src_ti && address == search->src_address &&",
	"      is_public)",
	"    search->src_public_in_whole = 1;",
	"  if (record == search->dst_ti && is_public)",
	"  {",
	"    long offset = 0;",
	"    if (cppgm_find_public_base(record, search->src_ti, 0, &offset) &&",
	"        address + offset == search->src_address)",
	"    {",
	"      if (search->result != address)",
	"      {",
	"        search->result_count = search->result_count + 1;",
	"        search->result = address;",
	"      }",
	"      return;",
	"    }",
	"  }",
	"  void * kind = cppgm_record_kind(record);",
	"  if (kind ==",
	"      cppgm_anchor_point(_ZTVN10__cxxabiv120__si_class_type_infoE))",
	"  {",
	"    void * base = *(void **)((char *)record + 16);",
	"    cppgm_cast_walk(search, base, address, is_public);",
	"    return;",
	"  }",
	"  if (kind ==",
	"      cppgm_anchor_point(_ZTVN10__cxxabiv121__vmi_class_type_infoE))",
	"  {",
	"    unsigned int count = *(unsigned int *)((char *)record + 20);",
	"    for (unsigned int i = 0; i < count; i++)",
	"    {",
	"      char * entry = (char *)record + 24 + 16 * (long)i;",
	"      void * base = *(void **)entry;",
	"      long offset_flags = *(long *)(entry + 8);",
	"      bool base_public = (offset_flags & 2) != 0;",
	"      cppgm_cast_walk(search, base, address + (offset_flags >> 8),",
	"                      is_public && base_public);",
	"    }",
	"  }",
	"}",
	"",
	"}  // namespace",
	"",
	"extern \"C\" void * __dynamic_cast(void * sub, void * src_ti,",
	"                                  void * dst_ti, long src2dst)",
	"{",
	"  if (sub == 0)",
	"    return 0;",
	"  char * vptr = *(char **)sub;",
	"  long offset_to_top = *(long *)(vptr - 16);",
	"  void * whole_ti = *(void **)(vptr - 8);",
	"  char * whole = (char *)sub + offset_to_top;",
	"  CppgmCastSearch search;",
	"  search.dst_ti = dst_ti;",
	"  search.src_ti = src_ti;",
	"  search.src_address = (char *)sub;",
	"  search.result = 0;",
	"  search.result_count = 0;",
	"  search.src_public_in_whole = 0;",
	"  cppgm_cast_walk(&search, whole_ti, whole, true);",
	"  if (search.result_count == 1)",
	"    return search.result;",
	"  // Cross cast: a unique public dst subobject when the source",
	"  // subobject itself is public in the complete object.",
	"  if (search.src_public_in_whole == 0)",
	"    return 0;",
	"  long dst_count = 0;",
	"  char * dst_address = 0;",
	"  long dst_offset = 0;",
	"  if (whole_ti == dst_ti)",
	"  {",
	"    dst_count = 1;",
	"    dst_address = whole;",
	"  }",
	"  else if (cppgm_find_public_base(whole_ti, dst_ti, 0, &dst_offset))",
	"  {",
	"    dst_count = 1;",
	"    dst_address = whole + dst_offset;",
	"  }",
	"  return dst_count == 1 ? dst_address : 0;",
	"}",
	"",
	"extern \"C\" void __cxa_bad_cast()",
	"{",
	"  __cppgm_exit(134);",
	"}",
	"",
	"extern \"C\" void __cxa_bad_typeid()",
	"{",
	"  __cppgm_exit(134);",
	"}",
	"",
	"extern \"C\" int __cxa_pure_virtual(void *)",
	"{",
	"  __cppgm_exit(134);",
	"  return 0;",
	"}",
	"",
	"namespace {",
	"",
	"enum { CPPGM_HEAP_WORDS = 1 << 18 };",
	"",
	"unsigned long cppgm_heap[CPPGM_HEAP_WORDS];",
	"cppgm_usize cppgm_heap_next;",
	"",
	"}  // namespace",
	"",
	"void * operator new(cppgm_usize size)",
	"{",
	"  cppgm_usize words = (size + 15) / 8 & ~(cppgm_usize)1;",
	"  if (words == 0)",
	"    words = 2;",
	"  if (cppgm_heap_next + words > (cppgm_usize)CPPGM_HEAP_WORDS)",
	"    __cppgm_exit(134);",
	"  void * result = &cppgm_heap[cppgm_heap_next];",
	"  cppgm_heap_next = cppgm_heap_next + words;",
	"  return result;",
	"}",
	"",
	"void * operator new[](cppgm_usize size)",
	"{",
	"  return operator new(size);",
	"}",
	"",
	"void operator delete(void *)",
	"{",
	"}",
	"",
	"void operator delete[](void *)",
	"{",
	"}",
	"",
	"extern \"C\" void * malloc(cppgm_usize size)",
	"{",
	"  return operator new(size);",
	"}",
	"",
	"extern \"C\" void free(void *)",
	"{",
	"}",
};

}  // namespace

const std::string & RuntimeLibrarySource()
{
	static std::string source;
	if (source.empty())
	{
		for (size_t i = 0;
		     i < sizeof(kRuntimeLines) / sizeof(kRuntimeLines[0]); i++)
		{
			source += kRuntimeLines[i];
			source += '\n';
		}
	}
	return source;
}

const char * RuntimeLibraryName()
{
	return "<cppgm++ runtime>";
}

namespace {

// The Itanium builtin-type encodings whose typeinfo the runtime owns
// (mirroring the libstdc++ fundamental_type_info exports).
const char * const kFundamentalEncodings[] = {
	"v", "w", "b", "c", "a", "h", "s", "t", "i", "j", "l", "m",
	"x", "y", "n", "o", "f", "d", "e", "Dn", "Di", "Ds",
};
const size_t kFundamentalCount =
	sizeof(kFundamentalEncodings) / sizeof(kFundamentalEncodings[0]);

bool IsFundamentalEncoding(const std::string & encoding)
{
	for (size_t i = 0; i < kFundamentalCount; i++)
		if (encoding == kFundamentalEncodings[i])
			return true;
	return false;
}

// Appends one defined typeinfo data item + weak symbol.
int AppendTypeinfoItem(ObjectModule & module, const std::string & name,
                       const std::vector<unsigned char> & bytes,
                       const std::vector<X86Patch> & patches)
{
	ImageItem item;
	item.align = 8;
	item.bytes = bytes;
	item.patches = patches;
	module.items.push_back(item);
	ObjectSymbol symbol;
	symbol.low_name = name;
	symbol.external_name = name;
	symbol.binding = ObjectSymbol::SB_WEAK;
	symbol.item = static_cast<int>(module.items.size()) - 1;
	module.symbols.push_back(symbol);
	return static_cast<int>(module.symbols.size()) - 1;
}

X86Patch MakeAbsPatch(size_t offset, int symbol, long long addend)
{
	X86Patch patch;
	patch.offset = offset;
	patch.size = 8;
	patch.kind = X86_PATCH_ABS;
	patch.imm = X86Imm::Label(symbol,
	                          static_cast<unsigned long long>(addend));
	return patch;
}

int FindModuleSymbol(const ObjectModule & module, const std::string & name)
{
	for (size_t s = 0; s < module.symbols.size(); s++)
		if (module.symbols[s].external_name == name &&
		    module.symbols[s].item >= 0)
			return static_cast<int>(s);
	throw std::runtime_error("runtime module lost anchor: " + name);
}

int AppendTypeinfoName(ObjectModule & module, const std::string & encoding)
{
	std::vector<unsigned char> bytes;
	for (size_t c = 0; c < encoding.size(); c++)
		bytes.push_back(static_cast<unsigned char>(encoding[c]));
	bytes.push_back(0);
	return AppendTypeinfoItem(module, "_ZTS" + encoding, bytes,
	                          std::vector<X86Patch>());
}

}  // namespace

bool IsRuntimeOwnedTypeinfoName(const std::string & name)
{
	if (name.size() < 5 || name.compare(0, 3, "_ZT") != 0 ||
	    (name[3] != 'I' && name[3] != 'S'))
		return false;
	std::string encoding = name.substr(4);
	if (!encoding.empty() && encoding[0] == 'P')
	{
		encoding = encoding.substr(1);
		if (!encoding.empty() && encoding[0] == 'K')
			encoding = encoding.substr(1);
	}
	return IsFundamentalEncoding(encoding);
}

// The Itanium records: fundamentals are {vptr, name}; pointer variants
// are __pointer_type_info {vptr, name, u32 flags, pad, pointee} - the
// layouts the private matcher in this file's runtime source reads.
void AppendRuntimeTypeinfo(ObjectModule & module)
{
	int fundamental_vtable = FindModuleSymbol(
		module, "_ZTVN10__cxxabiv123__fundamental_type_infoE");
	int pointer_vtable = FindModuleSymbol(
		module, "_ZTVN10__cxxabiv119__pointer_type_infoE");
	for (size_t i = 0; i < kFundamentalCount; i++)
	{
		const std::string encoding = kFundamentalEncodings[i];
		int name_symbol = AppendTypeinfoName(module, encoding);
		std::vector<unsigned char> record(16, 0);
		std::vector<X86Patch> patches;
		patches.push_back(MakeAbsPatch(0, fundamental_vtable, 16));
		patches.push_back(MakeAbsPatch(8, name_symbol, 0));
		int record_symbol = AppendTypeinfoItem(module, "_ZTI" + encoding,
		                                       record, patches);
		const char * const prefixes[2] = { "P", "PK" };
		const unsigned flags[2] = { 0, 1 };   // __const_mask pointee
		for (int p = 0; p < 2; p++)
		{
			std::string pointer_encoding = prefixes[p] + encoding;
			int pointer_name =
				AppendTypeinfoName(module, pointer_encoding);
			std::vector<unsigned char> pointer_record(32, 0);
			pointer_record[16] = static_cast<unsigned char>(flags[p]);
			std::vector<X86Patch> pointer_patches;
			pointer_patches.push_back(
				MakeAbsPatch(0, pointer_vtable, 16));
			pointer_patches.push_back(MakeAbsPatch(8, pointer_name, 0));
			pointer_patches.push_back(
				MakeAbsPatch(24, record_symbol, 0));
			AppendTypeinfoItem(module, "_ZTI" + pointer_encoding,
			                   pointer_record, pointer_patches);
		}
	}
}

}  // namespace toolchain
