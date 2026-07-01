// VALIDATION: compile-pass
// A qualified member class template inherited from a concrete specialization
// can feed a boolean non-type template argument.

template<bool B>
struct bool_
{
  static const bool value = B;
};

struct true_
{
  static const bool value = true;
  typedef true_ type;
};

template<class Tag>
struct impl;

struct base_tag
{
};

struct derived_tag
{
};

template<>
struct impl<base_tag>
{
  template<class T>
  struct apply : true_
  {
  };
};

template<>
struct impl<derived_tag> : impl<base_tag>
{
};

typedef bool_<(bool)impl<derived_tag>::template apply<int>::type::value> check;

int main()
{
  return check::value ? 0 : 1;
}
