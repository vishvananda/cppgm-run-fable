// VALIDATION: compile-pass
// Boost.Fusion reduction: a dependent qualified member return type keeps the
// class-template owner metadata even when an unrelated ordinary function has
// the same unqualified name as the class template.

int remove(char const *);

namespace boost
{
namespace fusion
{
namespace result_of
{
template<class Seq, class T>
struct remove
{
  typedef int type;
};
}

template<class T, class Seq>
typename result_of::remove<Seq const, T>::type remove(Seq const &)
{
  return 0;
}
}
}

struct X
{
};

int main()
{
  namespace fusion = boost::fusion;
  int value = 0;
  return fusion::remove<X>(value);
}
