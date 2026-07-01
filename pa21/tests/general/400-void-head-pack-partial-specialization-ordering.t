// VALIDATION: compile-pass

template<class... Types>
struct typelist
{
};

template<class Typelist>
struct do_pack;

template<>
struct do_pack<typelist<> >;

template<class Prev>
struct do_pack<typelist<Prev> >
{
  typedef Prev type;
};

template<class Prev, class Last>
struct do_pack<typelist<Prev, Last> >
{
  typedef Last type;
};

template<class... Others>
struct do_pack<typelist<void, Others...> >
{
  typedef typename do_pack<typelist<Others...> >::type type;
};

template<class Prev, class... Others>
struct do_pack<typelist<Prev, Others...> >
{
  typedef Prev type;
};

typedef do_pack<typelist<void, int, char> >::type result_t;
typedef do_pack<typelist<int, char> >::type direct_t;

int expect(char *);

int main()
{
  result_t *p = 0;
  direct_t *q = 0;
  return expect(p) + expect(q);
}
