template<class A, class B>
struct is_same { static const bool value = false; };

template<class A>
struct is_same<A, A> { static const bool value = true; };

template<class T>
struct remove_cv {
  typedef T type;
};

template<class T>
struct remove_cv<const T> {
  typedef T type;
};

template<class T>
struct remove_cv<volatile T> {
  typedef T type;
};

template<class T>
struct remove_cv<const volatile T> {
  typedef T type;
};

template<class T>
using remove_cv_t = typename remove_cv<T>::type;

template<class T>
struct iterator_traits;

template<class T>
struct iterator_traits<T*> { typedef remove_cv_t<T> value_type; };

template<class T, bool = true>
struct make_unsigned_impl {};

template<>
struct make_unsigned_impl<long, true> { typedef unsigned long type; };

template<class T>
using make_unsigned_t = typename make_unsigned_impl<remove_cv_t<T>>::type;

static_assert(is_same<typename iterator_traits<const char*>::value_type, char>::value,
              "remove_cv_t did not resolve after pointer partial specialization");
static_assert(is_same<make_unsigned_t<const long>, unsigned long>::value,
              "remove_cv_t did not resolve inside alias member lookup");

int main() { return 0; }
