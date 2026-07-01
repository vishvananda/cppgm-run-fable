namespace ns {
template<class T, T v>
struct integral_constant {
  static const T value = v;
};

typedef integral_constant<bool, true> true_type;

template<bool B>
struct enable_if {};

template<>
struct enable_if<true> {
  typedef void type;
};

template<bool B>
using enable_if_t = typename enable_if<B>::type;

template<class...>
using expand_to_true = true_type;

template<class... Pred>
using All = expand_to_true<enable_if_t<Pred::value>...>;
}

struct Function {
  template<class F,
           class = ns::enable_if_t<
               ns::All<ns::true_type, ns::true_type>::value> >
  Function& operator=(F&&);
};

template<class F, class>
Function& Function::operator=(F&&) {
  return *this;
}

struct S {
  struct G {};
  void g();
};

void S::g() {
  Function f;
  f = G();
}

int main() {
  S s;
  s.g();
}
