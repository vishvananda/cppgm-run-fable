// VALIDATION: compile-pass
// N3485 focus: 14.5.5 [temp.class.spec]; variable templates are post-N3485.

template<long N, long D>
struct ratio
{
};

template<class T>
const bool is_ratio = false;

template<long N, long D>
const bool is_ratio<ratio<N, D> > = true;

using nano = ratio<1, 1000>;

static_assert(is_ratio<nano>, "");

int main()
{
  return 0;
}
