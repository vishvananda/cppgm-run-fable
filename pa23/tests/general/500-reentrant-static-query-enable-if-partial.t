// VALIDATION: compile-pass
// Reentrant class-template partial selection must not cache a primary
// specialization while an enable_if_t value argument is still dependent.

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

namespace api {
namespace execution {
namespace detail {
namespace outstanding_work {
template<int I>
struct untracked_t {
};

template<int I>
struct tracked_t {
};
}
}

struct outstanding_work_t {
  typedef detail::outstanding_work::untracked_t<0> untracked_t;
  typedef detail::outstanding_work::tracked_t<0> tracked_t;
};
}

namespace traits {
template<class T, class Property, class = void>
struct query_free {
  static constexpr bool is_valid = false;
};

template<class T, class Property, class = void>
struct query_member {
  static constexpr bool is_valid = false;
};

template<class T, class Property, class = void>
struct static_query {
  static constexpr bool is_valid = false;
};
}

template<class T, class Property>
struct is_applicable_property {
  static constexpr bool value = false;
};

template<class T>
T && declval();

struct true_type {
  static constexpr bool value = true;
};

struct false_type {
  static constexpr bool value = false;
};

struct executor {
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
struct is_applicable_property<T, execution::outstanding_work_t::untracked_t> {
  static constexpr bool value = is_executor<T>::value;
};

template<class T>
struct is_applicable_property<T, execution::outstanding_work_t::tracked_t> {
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
struct static_query<T, execution::outstanding_work_t::untracked_t,
  enable_if_t<
    !query_member<T, execution::outstanding_work_t::untracked_t>::is_valid &&
    !query_free<T, execution::outstanding_work_t::untracked_t>::is_valid &&
    !can_query<T, execution::outstanding_work_t::tracked_t>::value
  > > {
  static constexpr bool is_valid = true;
};

template<class T>
struct static_query<T, execution::outstanding_work_t::tracked_t,
  enable_if_t<
    !query_member<T, execution::outstanding_work_t::tracked_t>::is_valid &&
    !query_free<T, execution::outstanding_work_t::tracked_t>::is_valid &&
    !static_query<T, execution::outstanding_work_t::untracked_t>::is_valid
  > > {
  static constexpr bool is_valid = true;
};
}
}

static_assert(api::traits::static_query<api::executor,
              api::execution::outstanding_work_t::untracked_t>::is_valid,
              "static_query should be valid");
static_assert(api::traits::static_query<const api::executor,
              api::execution::outstanding_work_t::untracked_t>::is_valid,
              "const static_query should be valid");
static_assert(api::query_fn::call_traits<api::query_fn::impl,
              api::executor,
              void(api::execution::outstanding_work_t::untracked_t)>::overload ==
              api::query_fn::static_value,
              "call traits should select static query");
static_assert(api::can_query<api::executor,
              api::execution::outstanding_work_t::untracked_t>::value,
              "can_query should use static query");

int main()
{
  return api::can_query<api::executor,
                        api::execution::outstanding_work_t::untracked_t>::value ? 0 : 1;
}
