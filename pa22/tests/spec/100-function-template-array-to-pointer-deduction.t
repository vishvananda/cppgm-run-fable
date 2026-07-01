// VALIDATION: compile-pass
// N3485 focus: 14.8.2 [temp.deduct]

template<class T>
T first(const T * p)
{
  return *p;
}

int main()
{
  return first("x") == 'x' ? 0 : 1;
}
