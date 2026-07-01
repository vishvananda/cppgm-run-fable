// VALIDATION: compile-pass
// Boost.Container reduction: a qualified enable_if return type can contain a
// bool expression that references a trait instantiated with
// typename Container::value_type. The trait namespace is not the lookup context
// for that dependent member type, so it must remain dependent until the
// candidate function template is substituted.

namespace boost
{
namespace container
{
namespace dtl
{

template<bool B, class T = void>
struct enable_if_c
{
};

template<class T>
struct enable_if_c<true, T>
{
  typedef T type;
};

template<class T>
struct is_pair
{
  static const bool value = false;
};

template<class T1, class T2>
struct pair
{
  T1 first;
  T2 second;
};

template<class T1, class T2>
struct is_pair<pair<T1, T2> >
{
  static const bool value = true;
};

}
}
}

namespace boost
{
namespace container
{
namespace test
{

struct EmplaceInt
{
  int value;
};

template<class Container>
typename boost::container::dtl::enable_if_c<
    !boost::container::dtl::is_pair<typename Container::value_type>::value,
    bool>::type
test_expected_container(const Container &, const EmplaceInt *, unsigned int,
                        unsigned int = 0)
{
  return true;
}

template<class Container>
typename boost::container::dtl::enable_if_c<
    boost::container::dtl::is_pair<typename Container::value_type>::value,
    bool>::type
test_expected_container(const Container &,
                        const boost::container::dtl::pair<EmplaceInt, EmplaceInt> *,
                        unsigned int)
{
  return false;
}

template<class Container>
bool test_emplace_back()
{
  Container c;
  EmplaceInt expected[1];
  return test_expected_container(c, &expected[0], 1);
}

template<class Container>
bool test_emplace()
{
  return test_emplace_back<Container>();
}

}
}
}

struct V
{
  typedef int value_type;
};

int main()
{
  return boost::container::test::test_emplace<V>() ? 0 : 1;
}
