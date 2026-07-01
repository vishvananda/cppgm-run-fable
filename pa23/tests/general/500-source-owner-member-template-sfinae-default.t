// VALIDATION: compile-pass
// An out-of-class static member definition can cause a dependent source-owner
// class template to collect member templates before a concrete instantiation
// uses them. A defaulted SFINAE parameter on those member templates must still
// be substituted against the concrete owner and dropped if it retains the
// member function template parameter.

template<class T>
struct dependent_true
{
  enum { value = true };
};

template<bool B, class T = void>
struct disable_if
{
  typedef T type;
};

template<class T>
struct disable_if<true, T>
{};

template<class C>
struct string_like
{
  typedef const C * const_iterator;
  static const int npos;

  template<class View>
  C *insert(View sv)
  {
    return insert((const_iterator)0, sv.data(), sv.data());
  }

  template<class InputIter>
  C *insert(const_iterator, InputIter, InputIter,
            typename disable_if<dependent_true<InputIter>::value>::type * = 0)
  {
    return (C *)0;
  }

  template<class ForwardIter>
  C *insert(const_iterator, ForwardIter, ForwardIter, void * = 0)
  {
    return (C *)0;
  }
};

template<class C>
const int string_like<C>::npos = 0;

struct view_like
{
  const char *data() const
  {
    return 0;
  }
};

int main()
{
  string_like<char> s;
  view_like v;
  return s.insert(v) == 0 ? 0 : 1;
}
