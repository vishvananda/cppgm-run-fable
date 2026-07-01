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

template<class Typelist, class Indexes>
struct invert_typelist_impl;

template<class Typelist, unsigned long... Ints>
struct invert_typelist_impl<Typelist, index_tuple<Ints...> >
{
  typedef Typelist type;
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

int main()
{
  return 0;
}
