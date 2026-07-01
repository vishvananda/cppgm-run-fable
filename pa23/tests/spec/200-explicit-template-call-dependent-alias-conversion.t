// VALIDATION: compile-pass
// N3485 focus: 14.8.1 [temp.arg.explicit], 14.6.2.1 [temp.dep.type]

namespace std {
inline namespace __1 {

struct classic_alg_policy {};

template<class P>
struct iter_ops
{
  template<class I>
  using difference_type = long;
};

template<class P, class I, class O>
int copy_n(I, typename iter_ops<P>::template difference_type<I>, O)
{
  return 7;
}

}
}

int main()
{
  return std::copy_n<std::classic_alg_policy>((const char *)0, 1, (char *)0) == 7 ? 0 : 1;
}
