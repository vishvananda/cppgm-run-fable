// VALIDATION: compile-pass
// N3485 focus: 14.7.2 [temp.explicit]

template<class T>
struct tester
{
  static int test();
};

template<class T>
int tester<T>::test()
{
  return 7;
}

template int tester<int>::test();

int main()
{
  return tester<int>::test() == 7 ? 0 : 1;
}
