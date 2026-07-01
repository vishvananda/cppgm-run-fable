// VALIDATION: compile-pass

template<class...>
struct common;

template<class T>
struct make_void
{
  typedef void type;
};

template<class...>
struct types;

template<class, class = void>
struct impl
{
};

template<class T, class U>
struct common<T, U>
{
  typedef long type;
};

template<class T, class U>
struct impl<types<T, U>, typename make_void<typename common<T, U>::type>::type>
{
  typedef typename common<T, U>::type type;
};

template<class T, class U, class V, class... Rest>
struct impl<types<T, U, V, Rest...>,
            typename make_void<typename common<T, U>::type>::type>
  : impl<types<typename common<T, U>::type, V, Rest...> >
{
};

template<class T, class U, class V, class... Rest>
struct common<T, U, V, Rest...> : impl<types<T, U, V, Rest...> >
{
};

typedef typename common<int, int, long>::type CT1;
typedef typename common<int, int, long, long>::type CT2;

CT1 g1;
CT2 g2;

int main()
{
  return g1 + g2;
}
