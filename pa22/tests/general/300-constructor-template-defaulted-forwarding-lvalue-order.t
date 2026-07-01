// VALIDATION: compile-pass
// A const lvalue should prefer the const-reference constructor template.

template<bool B, class T = void>
struct enable_if {};

template<class T>
struct enable_if<true, T> {
  typedef T type;
};

template<class A, class B>
struct is_same {
  static const bool value = false;
};

template<class A>
struct is_same<A, A> {
  static const bool value = true;
};

template<class T>
struct is_const {
  static const bool value = false;
};

template<class T>
struct is_const<const T> {
  static const bool value = true;
};

struct value {};

struct box {
  static int selected;

  template<class T>
  box(const T &) {
    selected = 1;
  }

  template<class T>
  box(T &&,
      typename enable_if<!is_same<box &, T>::value>::type * = 0,
      typename enable_if<!is_const<T>::value>::type * = 0) {
    selected = 2;
  }
};

int box::selected = 0;
value global_value;

const value &source() {
  return global_value;
}

int main() {
  box tmp(source());
  return box::selected - 1;
}
