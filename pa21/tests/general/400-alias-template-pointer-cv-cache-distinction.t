template<class A, class B>
struct is_same { static constexpr bool value = false; };

template<class A>
struct is_same<A, A> { static constexpr bool value = true; };

template<class T>
using add_lvalue_reference_t = T&;

using A = add_lvalue_reference_t<char* const>;
using B = add_lvalue_reference_t<const char*>;

static_assert(is_same<A, char* const&>::value, "A");
static_assert(is_same<B, const char*&>::value, "B");
static_assert(!is_same<A, B>::value, "distinct");

int main() { return 0; }
