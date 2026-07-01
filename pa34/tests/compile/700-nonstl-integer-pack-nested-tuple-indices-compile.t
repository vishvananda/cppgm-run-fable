// A dependent alias-template body whose qualifier is a class template-id whose
// first argument is a concrete type *name* (size_t) and whose second argument
// is an unresolvable __integer_pack expansion. Because the pack keeps the
// qualifier structurally dependent, resolving it goes through text-based scope
// lookup, where the "size_t" argument arrives with no carried argument syntax.
// Resolving that bare concrete type name must succeed via ordinary name lookup
// (it is unsigned long), not fail as "failed type template argument resolution".
// This mirrors libc++'s __make_indices_imp / __integer_sequence machinery used
// by std::map::operator[] (piecewise tuple construction).
typedef unsigned long size_t;
template <size_t...> struct tuple_indices {};
template <class IdxType, IdxType... Values>
struct integer_sequence {
  template <size_t Sp>
  using to_tuple_indices = tuple_indices<(Values + Sp)...>;
};
template <size_t Ep, size_t Sp>
using make_indices_imp =
    typename integer_sequence<size_t, __integer_pack(Ep - Sp)...>::template to_tuple_indices<Sp>;
typedef make_indices_imp<1, 0> indices;
int main() { return 0; }
