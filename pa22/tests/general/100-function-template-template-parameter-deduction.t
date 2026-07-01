// VALIDATION: compile-pass
// N3485 focus: 14.8.2.1 [temp.deduct.call]

namespace ttp_deduce
{

template<class T>
class box
{
};

template<class T, template<class> class U>
int pass(const U<T> &)
{
  return 0;
}

box<double> *get_box();

int test()
{
  return pass(*get_box());
}

}

int main()
{
  return ttp_deduce::test();
}
