template<unsigned long...>
struct index_tuple
{
  typedef index_tuple type;
};

template<class S1, class S2>
struct concat_index_tuple;

template<unsigned long... I1, unsigned long... I2>
struct concat_index_tuple<index_tuple<I1...>, index_tuple<I2...> >
  : index_tuple<I1..., (sizeof...(I1) + I2)...>
{
};

template<unsigned long N>
struct build_number_seq;

template<unsigned long N>
struct build_number_seq
  : concat_index_tuple<typename build_number_seq<N / 2>::type,
                       typename build_number_seq<N - N / 2>::type>::type
{
};

template<>
struct build_number_seq<0> : index_tuple<>
{
};

template<>
struct build_number_seq<1> : index_tuple<0>
{
};

template<class Allocator, class... Args>
struct proxy
{
  typedef typename build_number_seq<sizeof...(Args)>::type index_tuple_t;
};

typedef proxy<int, int, int, int>::index_tuple_t result_t;

int expect(index_tuple<0, 1, 2> *);

int main()
{
  result_t * p = 0;
  return expect(p);
}
