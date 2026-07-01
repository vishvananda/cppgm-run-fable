// VALIDATION: compile-pass
// N3485 focus: 14.5.1 [temp.class], 9.3 [class.mfct]

template<class T>
struct Box
{
  enum Tag { good };

  int call(Tag tag)
  {
    return value(tag);
  }

  int value(Tag tag);
};

template<class T>
int Box<T>::value(Tag)
{
  return 0;
}

int main()
{
  Box<int> box;
  return box.call(Box<int>::good);
}
