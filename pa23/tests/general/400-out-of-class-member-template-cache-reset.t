// VALIDATION: compile-pass
// A member-function template instantiated from an out-of-class definition while
// its owner class template is only reference-collected must be removed from the
// source template cache if that owner is reset before full collection.

template<class T>
T&& declval();

template<class T>
struct make_void {
  typedef void type;
};

template<class T>
using void_t = typename make_void<T>::type;

struct function_object {
};

template<class Allocator, unsigned Bits>
struct executor {
  typedef int marker;

  template<class Function>
  void execute(Function&&) const;
};

template<class Allocator, unsigned Bits>
template<class Function>
void executor<Allocator, Bits>::execute(Function&&) const
{
}

template<class T, class F, class = void>
struct execute_member_valid {
  static constexpr bool value = false;
};

template<class T, class F>
struct execute_member_valid<T, F,
    void_t<decltype(declval<T>().execute(declval<F>()))> > {
  static constexpr bool value = true;
};

template<class T, class F, class = void>
struct execute_member_noexcept {
  static constexpr bool value = false;
};

template<class T, class F>
struct execute_member_noexcept<T, F,
    void_t<decltype(declval<T>().execute(declval<F>()))> > {
  static constexpr bool value = noexcept(declval<T>().execute(declval<F>()));
};

typedef executor<int, 0>::marker force_reference_collection;

static_assert(execute_member_valid<const executor<int, 0>, function_object>::value,
              "member template should be valid through reference collection");

int force_full_collection = sizeof(executor<int, 0>);

static_assert(!execute_member_noexcept<const executor<int, 0>, function_object>::value,
              "execute is not noexcept after full collection");

int main()
{
  return force_full_collection == 0;
}
