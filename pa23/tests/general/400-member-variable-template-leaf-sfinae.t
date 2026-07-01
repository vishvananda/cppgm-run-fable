struct false_type {
  static constexpr bool value = false;
};

struct true_type {
  static constexpr bool value = true;
};

template<bool B, class T = void>
struct enable_if {
};

template<class T>
struct enable_if<true, T> {
  typedef T type;
};

template<bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

struct executor {
};

template<class T>
struct is_executor : false_type {
};

template<>
struct is_executor<executor> : true_type {
};

template<class T, class Property, class = void>
struct is_applicable_property : false_type {
};

template<class T, class Property>
struct is_applicable_property<
    T,
    Property,
    enable_if_t<!!Property::template is_applicable_property_v<T> > >
  : true_type {
};

template<int I = 0>
struct property {
  template<class T>
  static constexpr bool is_applicable_property_v = is_executor<T>::value;
};

template<class T, class Property, class = void>
struct selected {
  static constexpr int value = 1;
};

template<class T, class Property>
struct selected<T, Property,
                enable_if_t<is_applicable_property<T, Property>::value> > {
  static constexpr int value = 0;
};

int main() {
  return selected<executor, property<> >::value;
}
