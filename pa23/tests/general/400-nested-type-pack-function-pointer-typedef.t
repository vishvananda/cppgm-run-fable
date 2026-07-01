template<class T>
struct add_value {
  typedef T type;
};

template<class... T>
struct list {
};

template<class A, class B>
struct is_same {
  static const bool value = false;
};

template<class A>
struct is_same<A, A> {
  static const bool value = true;
};

template<class... A>
struct list_av {
  typedef list<typename add_value<A>::type...> type;
};

typedef bool (*fn)();
typedef list_av<fn>::type R;
typedef list<fn> Expected;

int main()
{
  return is_same<R, Expected>::value ? 0 : 1;
}
