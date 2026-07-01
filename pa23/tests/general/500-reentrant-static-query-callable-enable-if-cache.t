// VALIDATION: compile-pass
// A cached function-template return type that contains enable_if_t must be
// revalidated after reentrant static-query partial selection becomes concrete.

template<bool B, class T = void>
struct enable_if {
};

template<class T>
struct enable_if<true, T> {
  typedef T type;
};

template<bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

template<class T>
struct decay {
  typedef T type;
};

template<class T>
struct decay<const T> {
  typedef T type;
};

template<class T>
struct decay<T&> {
  typedef T type;
};

template<class T>
struct decay<const T&> {
  typedef T type;
};

template<class T>
using decay_t = typename decay<T>::type;

template<class T>
T && declval();

template<class A, class B>
struct is_same {
  static constexpr bool value = false;
};

template<class A>
struct is_same<A, A> {
  static constexpr bool value = true;
};

namespace api {
namespace execution {
namespace detail {
namespace relationship {
template<int I>
struct fork_t {
};

template<int I>
struct continuation_t {
};
}
}

struct relationship_t {
  typedef detail::relationship::fork_t<0> fork_t;
  typedef detail::relationship::continuation_t<0> continuation_t;
};
}

namespace traits {
template<class T, class Property, class = void>
struct query_free {
  static constexpr bool is_valid = false;
  typedef void result_type;
};

template<class T, class Property, class = void>
struct query_member {
  static constexpr bool is_valid = false;
  typedef void result_type;
};

template<class T, class Property>
struct query_member<T, Property,
  enable_if_t<
    is_same<decltype(declval<T>().query(declval<Property>())),
            decltype(declval<T>().query(declval<Property>()))>::value
  > > {
  static constexpr bool is_valid = true;
  typedef decltype(declval<T>().query(declval<Property>())) result_type;
};

template<class T, class Property, class = void>
struct static_query {
  static constexpr bool is_valid = false;
  typedef void result_type;
};
}

template<class T, class Property>
struct is_applicable_property {
  static constexpr bool value = false;
};

struct true_type {
  static constexpr bool value = true;
};

struct false_type {
  static constexpr bool value = false;
};

struct executor {
  execution::relationship_t::fork_t query(execution::relationship_t::fork_t) const
  {
    return execution::relationship_t::fork_t();
  }

  template<class F>
  void execute(const F &) const
  {
  }
};

template<class T>
struct is_executor {
  template<class U>
  static true_type test(decltype(declval<U>().execute(declval<void (*)()>())) *);

  template<class>
  static false_type test(...);

  static constexpr bool value = decltype(test<T>(0))::value;
};

template<class T>
struct is_applicable_property<T, execution::relationship_t::fork_t> {
  static constexpr bool value = is_executor<T>::value;
};

template<class T>
struct is_applicable_property<T, execution::relationship_t::continuation_t> {
  static constexpr bool value = is_executor<T>::value;
};

namespace query_fn {
using ::decay_t;
using ::enable_if_t;
using api::is_applicable_property;
using api::traits::query_free;
using api::traits::query_member;
using api::traits::static_query;

enum overload_type {
  static_value,
  call_member,
  call_free,
  ill_formed
};

template<class Impl, class T, class Properties,
         class = void, class = void, class = void, class = void>
struct call_traits {
  static constexpr overload_type overload = ill_formed;
  typedef void result_type;
};

template<class Impl, class T, class Property>
struct call_traits<Impl, T, void(Property),
  enable_if_t<is_applicable_property<decay_t<T>, decay_t<Property> >::value>,
  enable_if_t<static_query<T, Property>::is_valid> > :
  static_query<T, Property> {
  static constexpr overload_type overload = static_value;
};

template<class Impl, class T, class Property>
struct call_traits<Impl, T, void(Property),
  enable_if_t<is_applicable_property<decay_t<T>, decay_t<Property> >::value>,
  enable_if_t<!static_query<T, Property>::is_valid>,
  enable_if_t<query_member<T, Property>::is_valid> > :
  query_member<T, Property> {
  static constexpr overload_type overload = call_member;
};

template<class Impl, class T, class Property>
struct call_traits<Impl, T, void(Property),
  enable_if_t<is_applicable_property<decay_t<T>, decay_t<Property> >::value>,
  enable_if_t<!static_query<T, Property>::is_valid>,
  enable_if_t<!query_member<T, Property>::is_valid>,
  enable_if_t<query_free<T, Property>::is_valid> > :
  query_free<T, Property> {
  static constexpr overload_type overload = call_free;
};

struct impl {
  template<class T, class Property>
  enable_if_t<
    call_traits<impl, T, void(Property)>::overload == static_value,
    typename call_traits<impl, T, void(Property)>::result_type
  >
  operator()(T &&, Property &&) const
  {
    return static_query<T, Property>::value();
  }

  template<class T, class Property>
  enable_if_t<
    call_traits<impl, T, void(Property)>::overload == call_member,
    typename call_traits<impl, T, void(Property)>::result_type
  >
  operator()(T && t, Property && p) const
  {
    return static_cast<T &&>(t).query(static_cast<Property &&>(p));
  }
};
}

template<class T, class Property>
struct can_query {
  static constexpr bool value =
      query_fn::call_traits<query_fn::impl, T, void(Property)>::overload !=
      query_fn::ill_formed;
};

namespace traits {
template<class T>
struct static_query<T, execution::relationship_t::fork_t,
  enable_if_t<
    !query_member<T, execution::relationship_t::fork_t>::is_valid &&
    !query_free<T, execution::relationship_t::fork_t>::is_valid &&
    !can_query<T, execution::relationship_t::continuation_t>::value
  > > {
  static constexpr bool is_valid = true;
  typedef execution::relationship_t::fork_t result_type;

  static result_type value()
  {
    return result_type();
  }
};

template<class T>
struct static_query<T, execution::relationship_t::continuation_t,
  enable_if_t<
    !query_member<T, execution::relationship_t::continuation_t>::is_valid &&
    !query_free<T, execution::relationship_t::continuation_t>::is_valid &&
    !static_query<T, execution::relationship_t::fork_t>::is_valid
  > > {
  static constexpr bool is_valid = true;
  typedef execution::relationship_t::continuation_t result_type;

  static result_type value()
  {
    return result_type();
  }
};
}
}

int main()
{
  api::query_fn::impl query;
  api::execution::relationship_t::fork_t result =
      query(api::executor(), api::execution::relationship_t::fork_t());
  return api::can_query<api::executor,
                        api::execution::relationship_t::fork_t>::value ? 0 : 1;
}
