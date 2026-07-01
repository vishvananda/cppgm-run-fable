// VALIDATION: compile-pass
// N3485 focus: 14.8.2 [temp.deduct], 14.8.3 [temp.over]
// Reducer for libstdc++ 15 allocator_traits: a member function template
// constrains its return type with a static variable template inherited from
// a base class.

template<bool B, class T = void>
struct enable_if
{
};

template<class T>
struct enable_if<true, T>
{
  typedef T type;
};

template<bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

struct traits_base
{
protected:
  template<class Alloc, class T, class... Args>
  static const bool can_construct = true;
};

template<class Alloc>
struct traits : traits_base
{
  template<class T, class... Args>
  static enable_if_t<can_construct<Alloc, T, Args...> >
  construct(Alloc&, T*, Args&&...)
  {
  }
};

struct Alloc
{
};

struct X
{
};

int main()
{
  Alloc a;
  X* p = 0;
  traits<Alloc>::construct(a, p, 1);
  return 0;
}
