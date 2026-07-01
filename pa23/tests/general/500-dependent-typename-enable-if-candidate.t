// VALIDATION: compile-pass
// Boost.Container reduction: while instantiating a template body, overload
// resolution examines an enable_if candidate whose return type contains a
// dependent typename template argument. The candidate must remain dependent
// until its own template parameters are substituted.

namespace boost
{
namespace move_detail
{

struct enable_if_nat
{
};

template<bool B, class T = enable_if_nat>
struct enable_if_c
{
  typedef T type;
};

template<class T>
struct enable_if_c<false, T>
{
};

template<class T>
struct remove_reference
{
  typedef T type;
};

template<class T>
struct remove_reference<T &>
{
  typedef T type;
};

template<class T>
struct is_reference
{
  static const bool value = false;
};

}

namespace container
{
namespace dtl
{

using boost::move_detail::enable_if_c;
using boost::move_detail::remove_reference;

template<class T1, class T2>
struct pair
{
  T1 first;
  T2 second;
};

template<class T>
struct is_pair
{
  static const bool value = false;
};

template<class T1, class T2>
struct is_pair<pair<T1, T2> >
{
  static const bool value = true;
};

}
}
}

template<class T>
int helper(T *p, T &u)
{
  (void)p;
  (void)u;
  return 3;
}

template<class T, class U>
typename boost::container::dtl::enable_if_c<
    boost::container::dtl::is_pair<
        typename boost::container::dtl::remove_reference<T>::type>::value &&
        !boost::move_detail::is_reference<U>::value,
    int>::type
helper(T *p, U &u)
{
  (void)p;
  (void)u;
  return 7;
}

template<class T, class U>
int outer(T *p, U &u)
{
  return helper(&p->first, u.first);
}

int main()
{
  boost::container::dtl::pair<int, int> a;
  boost::container::dtl::pair<int, int> b;
  return outer(&a, b);
}
