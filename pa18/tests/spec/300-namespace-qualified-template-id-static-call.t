// VALIDATION: compile-pass
// N3485 focus: 14.2 [temp.names], 14.6.2.1 [temp.dep.type], 14.8 [temp.fct.spec]

namespace api {
template<class T>
struct traits
{
  static int pointer_to(T * p)
  {
    return *p + 1;
  }
};
}

template<class T>
int call_pointer_to(T * p)
{
  return api::traits<T>::pointer_to(p);
}

int main()
{
  int value = 4;
  return call_pointer_to(&value) - 5;
}
