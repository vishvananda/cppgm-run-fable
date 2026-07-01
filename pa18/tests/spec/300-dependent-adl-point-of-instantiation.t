// VALIDATION: compile-pass
// N3485 focus: 14.6.4 [temp.dep.res]

namespace lib {

template<typename T>
struct tag
{
};

}  // namespace lib

template<typename T>
int call_pick(T t)
{
  return pick(t);
}

namespace lib {

int pick(tag<int>)
{
  return 5;
}

}  // namespace lib

int main()
{
  return call_pick(lib::tag<int>()) == 5 ? 0 : 1;
}
