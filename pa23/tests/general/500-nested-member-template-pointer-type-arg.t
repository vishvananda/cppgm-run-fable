// VALIDATION: compile-pass
// N3485 focus: 14.3.1 [temp.arg.type], 14.6.2.1 [temp.dep.type]

template<bool B>
struct bool_constant
{
  static const bool value = B;
};

typedef bool_constant<false> false_type;

template<typename Pred>
struct not_ : bool_constant<!Pred::value> {};

template<typename T>
struct outer
{
  template<typename U>
  struct inner : false_type {};
};

int main()
{
  typedef not_<outer<int *>::inner<int *&> > trigger;
  return trigger::value ? 0 : 1;
}
