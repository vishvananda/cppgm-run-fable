// N3485 focus: 14.5.7 alias templates and 14.8.2 function-template substitution.
// A dependent function-template return type can pass through an alias template
// whose target is a member alias template of a dependent class-template-id.
template<class From>
struct copy_cv {
  template<class To>
  using apply = To;
};

template<class From, class To>
using copy_cv_t = typename copy_cv<From>::template apply<To>;

template<class T>
struct make_unsigned {};

template<>
struct make_unsigned<long> {
  typedef unsigned long type;
};

template<class T>
using make_unsigned_t = copy_cv_t<T, typename make_unsigned<T>::type>;

template<class T>
make_unsigned_t<T> to_unsigned_like(T value)
{
  return static_cast<make_unsigned_t<T> >(value);
}

int main()
{
  return to_unsigned_like(5L) == 5UL ? 0 : 1;
}
