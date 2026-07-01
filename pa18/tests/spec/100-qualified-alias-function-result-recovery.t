// VALIDATION: compile-pass
// N3485 focus: 14.6.2.1 [temp.dep.type], 14.8 [temp.fct.spec]

namespace api {
template<class T>
struct text
{
  int value;

  text() : value(1)
  {
  }
};

typedef text<char> string;

enum tag
{
  ok
};

template<class A, class B>
struct pair
{
  A first;
  B second;

  pair(A a, B b) : first(a), second(b)
  {
  }
};

template<class T>
pair<const api::string, tag> make_pairish(T value)
{
  (void)value;
  return pair<const api::string, tag>(api::string(), ok);
}
}

int main()
{
  api::pair<const api::string, api::tag> p = api::make_pairish(0);
  return p.first.value - 1;
}
