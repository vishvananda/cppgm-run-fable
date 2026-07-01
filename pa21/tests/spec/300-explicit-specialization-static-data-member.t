// VALIDATION: compile-pass
// N3485 focus: 14.7.3 [temp.expl.spec]

template<class T>
struct code
{
  static const int value;
};

template<class T>
const int code<T>::value = 1;

template<>
const int code<int>::value = 7;

int main()
{
  return code<char>::value == 1 && code<int>::value == 7 ? 0 : 1;
}
