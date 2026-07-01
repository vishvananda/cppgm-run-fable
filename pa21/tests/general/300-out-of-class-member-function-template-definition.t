template<class T>
struct box
{
  template<class U>
  int value(U);
};

template<class T>
template<class U>
int box<T>::value(U)
{
  return sizeof(T) == sizeof(char) && sizeof(U) == sizeof(int) ? 0 : 1;
}

int main()
{
  box<char> *b = 0;
  return b->value(0);
}
