// VALIDATION: compile-pass
// Regression: substituting a type parameter into a dependent qualifier can
// expand one source qualifier into a qualified template-id. The semantic
// qualifier type must still be used when evaluating a type trait.

namespace std {

template <class From, class To>
struct is_convertible {
  static const bool value = true;
};

template <class T>
struct Vec {
};

template <class T>
struct Unique {
  typedef T * pointer;
};

}

template <bool B>
using BoolAlias = void;

template <class UPtr, class U>
using Alias = BoolAlias<std::is_convertible<typename UPtr::pointer, U *>::value>;

template <class U,
          class = Alias<std::Unique<U>, U> >
void take(std::Unique<U> *)
{
}

int main()
{
  std::Unique<std::Vec<int> > value;
  take(&value);
  return 0;
}
