typedef unsigned long size_t;

template<class T, T... I>
struct integer_sequence {};

template<size_t... I>
using index_sequence = integer_sequence<size_t, I...>;

struct piecewise_construct_t {};

piecewise_construct_t piecewise_construct;

template<class... T>
struct tuple {};

template<class First, class Second>
struct pair {
  template<class... A, class... B>
  pair(piecewise_construct_t pc, tuple<A...> first_args, tuple<B...> second_args)
      : pair(pc, first_args, second_args, index_sequence<0>(), index_sequence<0>()) {}

private:
  template<class... A, class... B, size_t... I1, size_t... I2>
  pair(piecewise_construct_t,
       tuple<A...>&,
       tuple<B...>&,
       index_sequence<I1...>,
       index_sequence<I2...>) {}
};

int main() {
  tuple<int const &> first;
  tuple<char const &> second;
  pair<int const, char> p(piecewise_construct, first, second);
  (void)p;
  return 0;
}
