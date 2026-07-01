// VALIDATION: compile-pass
// Boost.Config BOOST_NO_CXX11_SFINAE_EXPR reduction.

template<class>
struct sfinae_primary_ignore
{
  typedef void type;
};

template<class T>
T& sfinae_primary_object();

template<class T, class E = void>
struct sfinae_primary_trait
{
  static const int value = 0;
};

template<class T>
struct sfinae_primary_trait<T,
    typename sfinae_primary_ignore<
        decltype(&sfinae_primary_object<T>())>::type>
{
};

template<class T>
struct sfinae_primary_result
{
  static const int value = T::value;
};

class sfinae_primary_type
{
  void operator&() const { }
};

int main()
{
  return sfinae_primary_result<
      sfinae_primary_trait<sfinae_primary_type> >::value;
}
