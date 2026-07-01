// VALIDATION: compile-pass
// A constructor parameter pack can appear before a defaulted non-type guard.

struct first_arg {};
struct second_arg {};

template<bool B, class T = void>
struct enable_if {};

template<class T>
struct enable_if<true, T> {
  typedef T type;
};

template<class... Ts>
struct type_list {};

struct true_type {
  static constexpr bool value = true;
};

template<class FromList, class ToList>
struct all_convertible;

template<class... From, class... To>
struct all_convertible<type_list<From...>, type_list<To...> > : true_type {};

template<class... Ts>
struct tuple_like {
  int selected;

  template<class... Args>
  using are_convertible = all_convertible<type_list<Args...>, type_list<Ts...> >;

  template<class... Args,
           typename enable_if<are_convertible<Args&&...>::value>::type * = nullptr>
  tuple_like(Args&&...) : selected(2) {}
};

int main() {
  first_arg first;
  second_arg second;
  tuple_like<first_arg, second_arg> value{first, second};
  return value.selected == 2 ? 0 : 1;
}
