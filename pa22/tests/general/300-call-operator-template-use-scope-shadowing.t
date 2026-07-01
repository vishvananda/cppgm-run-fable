namespace N
{
namespace algorithm
{
struct copy
{
};
}

template<bool Cond, class T = int>
struct enable_if
{
};

template<class T>
struct enable_if<true, T>
{
  typedef T type;
};

template<class AlgTag, class A, class B>
struct specialized_algorithm
{
  static const bool has = false;
};

struct copy_impl
{
  template<class In, class Sent, class Out,
           typename enable_if<
               !specialized_algorithm<algorithm::copy, In, Out>::has,
               int>::type = 0>
  int operator()(In, Sent, Out) const
  {
    return 7;
  }
};

template<class Algorithm, class In, class Sent, class Out>
int run_algorithm(In first, Sent last, Out out)
{
  return Algorithm()(first, last, out);
}
}

int main()
{
  return N::run_algorithm<N::copy_impl>(1, 2, 3) == 7 ? 0 : 1;
}
