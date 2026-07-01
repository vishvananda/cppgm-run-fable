// VALIDATION: compile-pass
// N3485 focus: 14.8.2 [temp.deduct], 3.4.2 [basic.lookup.argdep],
// and 5.3.7 [expr.unary.noexcept]. Boost.Asio query_free reduction:
// dependent decltype/noexcept probes must use ADL at the template definition
// site, ignore a later CPO object for ordinary lookup, and keep viable hidden
// friends that need a user-defined conversion.

template<class...>
struct make_void
{
  typedef void type;
};

template<class... T>
using void_t = typename make_void<T...>::type;

template<bool B, class T = void>
struct enable_if
{};

template<class T>
struct enable_if<true, T>
{
  typedef T type;
};

template<class A, class B>
struct is_same
{
  static const bool value = false;
};

template<class A>
struct is_same<A, A>
{
  static const bool value = true;
};

template<class T>
T&& declval();

namespace api
{
namespace traits
{
template<class T, class P, class = void>
struct query_free;
}

namespace detail
{
struct no_query_free
{
  static const bool is_valid = false;
  static const bool is_noexcept = false;
};

template<class T, class P, class = void>
struct query_free_trait : no_query_free
{};

template<class T, class P>
struct query_free_trait<T, P,
    void_t<decltype(query(declval<T>(), declval<P>()))> >
{
  static const bool is_valid = true;
  using result_type = decltype(query(declval<T>(), declval<P>()));
  static const bool is_noexcept = noexcept(query(declval<T>(), declval<P>()));
};
}

namespace traits
{
template<class T, class P, class X>
struct query_free : detail::query_free_trait<T, P>
{};
}

namespace query_fn
{
using traits::query_free;

void query();

enum overload_type
{
  call_free,
  ill_formed
};

template<class Impl, class T, class Properties, class = void>
struct call_traits
{
  static const overload_type overload = ill_formed;
  typedef void result_type;
};

template<class Impl, class T, class P>
struct call_traits<Impl, T, void(P),
    typename enable_if<query_free<T, P>::is_valid>::type>
{
  static const overload_type overload = call_free;
  typedef int result_type;
};

struct impl
{
  template<class T, class P>
  typename enable_if<
    call_traits<impl, T, void(P)>::overload == call_free,
    typename call_traits<impl, T, void(P)>::result_type
  >::type
  operator()(T&& t, P&& p) const
  {
    return query(static_cast<T&&>(t), static_cast<P&&>(p));
  }
};
}
}

struct subprop
{};

struct exec
{};

template<class T, class P>
struct can_query
{
  static const bool value = false;
};

template<>
struct can_query<const exec&, subprop>
{
  static const bool value = true;
};

struct prop
{
  struct convertible
  {
    convertible(prop)
    {}
  };

  template<class Executor>
  friend int query(const Executor&, convertible,
      typename enable_if<can_query<const Executor&, subprop>::value>::type* = 0)
  {
    return 1;
  }
};

namespace api
{
namespace
{
struct late_query_cpo
{
  template<class T, class P>
  char operator()(T&&, P&&) const;
};

static const late_query_cpo query = {};
}
}

typedef api::traits::query_free<const exec&, const prop&> query_probe;

static_assert(query_probe::is_valid, "hidden friend should be found by ADL");
static_assert(is_same<query_probe::result_type, int>::value,
    "late query CPO object must not supply the decltype result");
static_assert(!query_probe::is_noexcept, "ordinary query is not noexcept");

int main()
{
  return 0;
}
