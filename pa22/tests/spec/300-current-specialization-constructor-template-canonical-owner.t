template<bool B, class T = void>
struct enable_if {};

template<class T>
struct enable_if<true, T> { typedef T type; };

template<class A, class B>
struct is_same { static const bool value = false; };

template<class A>
struct is_same<A, A> { static const bool value = true; };

template<class Rep>
struct duration_like {
  Rep value;

  template<class Rep2>
  explicit duration_like(Rep2 r) : value(r) {}

  template<class Rep2,
           class = typename enable_if<!is_same<Rep, Rep2>::value, void>::type>
  duration_like(const duration_like<Rep2>& other) : value(other.value) {}

  template<class Rep2>
  static duration_like cast(const duration_like<Rep2>& other) {
    return duration_like(other);
  }
};

duration_like<long long> *source_duration();

int main() {
  return duration_like<int>::cast(*source_duration()).value - 7;
}
