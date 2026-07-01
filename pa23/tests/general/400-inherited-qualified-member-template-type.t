// VALIDATION: compile-pass
// Reduced from Boost.MPL size<>: an intermediate qualified template-id may name
// a member class template inherited from a concrete base specialization.

template<int N>
struct int_
{
  static const int value = N;
  typedef int_ type;
};

struct vector_tag
{
};

struct vector3
{
  typedef vector_tag tag;
  typedef int_<3> size;
};

template<class Sequence>
struct sequence_tag
{
  typedef typename Sequence::tag type;
};

template<class Tag>
struct size_base;

template<>
struct size_base<vector_tag>
{
  template<class Sequence>
  struct apply : Sequence::size
  {
  };
};

template<class Tag>
struct size_impl;

template<>
struct size_impl<vector_tag> : size_base<vector_tag>
{
};

typedef typename size_impl<
    typename sequence_tag<vector3>::type>::template apply<vector3>::type
    vector_size;

static_assert(vector_size::value == 3, "inherited member template type");

int main()
{
  return vector_size::value == 3 ? 0 : 1;
}
