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

template<class T, class... Ignored>
struct leaf {
  typedef T type;
};

template<class L, class... Rest>
struct impl;

template<class... T, class... Rest>
struct impl<list<T...>, Rest...> {
  typedef list<typename leaf<T, Rest...>::type...> type;
};

struct A {
};

struct B {
};

typedef typename impl<list<A, B> >::type R;
typedef list<A, B> Expected;

int main()
{
  return is_same<R, Expected>::value ? 0 : 1;
}
