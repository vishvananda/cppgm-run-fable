// VALIDATION: compile-pass
// A fixed-arity template-template partial specialization is more specialized
// than the same fixed prefix followed by a trailing template-argument pack.

template <typename Ptr, typename U, unsigned int RebindMode>
struct pointer_rebinder;

template <template <class, class...> class Ptr, typename A, class... An, class U>
struct pointer_rebinder<Ptr<A, An...>, U, 0u>
{
  typedef Ptr<U, An...> type;
  static const int value = 1;
};

template <template <class> class Ptr, typename A, class U>
struct pointer_rebinder<Ptr<A>, U, 0u>
{
  typedef Ptr<U> type;
  static const int value = 2;
};

template<class T>
struct alloc
{
};

int main()
{
  return pointer_rebinder<alloc<int>, void, 0u>::value == 2 ? 0 : 1;
}
