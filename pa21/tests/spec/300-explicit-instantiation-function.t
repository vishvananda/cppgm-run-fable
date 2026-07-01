// VALIDATION: compile-pass
// N3485 focus: 14.7.2 [temp.explicit]

template<typename T>
T plus_one(T value)
{
  return value + 1;
}

extern template int plus_one<int>(int);
template int plus_one<int>(int);

int main()
{
  return plus_one(4) == 5 ? 0 : 1;
}
