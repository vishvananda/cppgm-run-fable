// VALIDATION: compile-pass
// N3485 focus: 14.8.2.4 [temp.deduct.partial]

template<bool B, class T = void>
struct enable_if
{
};

template<class T>
struct enable_if<true, T>
{
  typedef T type;
};

template<class A, class B>
struct is_same
{
  static const bool value = false;
};

template<class A>
struct is_same<A, A>
{
  static const bool value = true;
};

struct any
{
};

template<class T>
struct in_place_type_t
{
};

struct any_like
{
  template<class T>
  any_like(T&&, typename enable_if<!is_same<T&&, any&&>::value>::type * = 0)
    : selected(1)
  {
  }

  template<class T, class... Args>
  explicit any_like(in_place_type_t<T>, Args&&...)
    : selected(2)
  {
  }

  int selected;
};

int main()
{
  in_place_type_t<int> tag;
  any_like value(tag);
  return value.selected == 2 ? 0 : 1;
}
