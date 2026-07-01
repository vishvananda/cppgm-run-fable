template<unsigned long... Indexes>
struct index_tuple
{
};

template<unsigned long Num, class Tuple = index_tuple<> >
struct build_number_seq;

template<unsigned long Num, unsigned long... Indexes>
struct build_number_seq<Num, index_tuple<Indexes...> >
  : build_number_seq<Num - 1, index_tuple<Indexes..., sizeof...(Indexes)> >
{
};

template<unsigned long... Indexes>
struct build_number_seq<0, index_tuple<Indexes...> >
{
  typedef index_tuple<Indexes...> type;
};

template<class... Types>
struct typelist
{
};

template<class List>
struct sizeof_typelist;

template<class... Types>
struct sizeof_typelist<typelist<Types...> >
{
  static const unsigned long value = sizeof...(Types);
};

template<unsigned long Index, class List>
struct typelist_element;

template<class Head, class... Tail>
struct typelist_element<0, typelist<Head, Tail...> >
{
  typedef Head type;
};

template<unsigned long Index, class Head, class... Tail>
struct typelist_element<Index, typelist<Head, Tail...> >
  : typelist_element<Index - 1, typelist<Tail...> >
{
};

template<class Typelist, class Indexes>
struct invert_typelist_impl;

template<class Typelist, unsigned long... Ints>
struct invert_typelist_impl<Typelist, index_tuple<Ints...> >
{
  static const unsigned long last_idx = sizeof_typelist<Typelist>::value - 1;
  typedef typelist<typename typelist_element<last_idx - Ints, Typelist>::type...> type;
};

template<class Typelist>
struct invert_typelist;

template<class... Types>
struct invert_typelist<typelist<Types...> >
{
  typedef typelist<Types...> typelist_t;
  typedef typename build_number_seq<sizeof...(Types)>::type indexes_t;
  typedef typename invert_typelist_impl<typelist_t, indexes_t>::type type;
};

typedef invert_typelist<typelist<int, char, long> >::type result_t;

int expect(typelist<long, char, int> *);

int main()
{
  result_t * p = 0;
  return expect(p);
}
