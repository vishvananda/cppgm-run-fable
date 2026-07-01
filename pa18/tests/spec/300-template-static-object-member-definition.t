// VALIDATION: compile-pass
// N3485 focus: 14.5.1.3 [temp.static]

template<class T>
struct static_constant
{
  static T value;
};

template<class T>
T static_constant<T>::value;

struct keyword
{
  int tag;
};

keyword *use_value()
{
  return &static_constant<keyword>::value;
}

int main()
{
  return use_value() ? 0 : 1;
}
