// VALIDATION: compile-pass

template<class T>
struct hash
{
  hash() = delete;
};

template<class T, class D>
struct unique_ptr
{
  typedef T *pointer;
};

template<class...>
struct all
{
  typedef void type;
};

template<class Type, class>
using enable_hash_helper_imp = Type;

template<class Type, class... Keys>
using enable_hash_helper =
    enable_hash_helper_imp<Type, typename all<Keys...>::type>;

template<class T, class D>
struct hash<enable_hash_helper<unique_ptr<T, D>, typename unique_ptr<T, D>::pointer> >
{
  enum { value = 1 };
};

int main()
{
  static_assert(hash<unique_ptr<int, int> >::value == 1, "");
  return hash<unique_ptr<int, int> >::value == 1 ? 0 : 1;
}
