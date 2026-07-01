// VALIDATION: compile-pass
// N3485 focus: 14.5.2 [temp.mem]

struct input_tag {};
struct forward_tag : input_tag {};

template<class T>
struct box
{
  template<class It>
  int pick(It, It, input_tag);

  template<class It>
  int pick(It, It, forward_tag);
};

template<class T>
template<class It>
int box<T>::pick(It, It, input_tag)
{
  return 1;
}

template<class T>
template<class It>
int box<T>::pick(It, It, forward_tag)
{
  return 2;
}

int main()
{
  box<int> b;
  return b.pick((char*)0, (char*)0, forward_tag()) == 2 ? 0 : 1;
}
