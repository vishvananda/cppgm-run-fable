template<bool B, class T = void>
struct enable_if {
};

template<class T>
struct enable_if<true, T> {
  typedef T type;
};

template<bool>
struct _TCC {
  template<class... Args>
  static constexpr bool __is_implicitly_constructible() {
    return false;
  }
};

template<class... _Elements>
struct tupleish {
  template<class... _Args,
           typename enable_if<
               _TCC<
                   sizeof...(_Args) == sizeof...(_Elements)
                   >::template __is_implicitly_constructible<_Args...>(),
               int>::type = 0>
  static int make(_Args...) {
    return 11;
  }

  static int make(...) {
    return 4;
  }
};

int main() {
  return tupleish<int>::make(1) - 11;
}
