// VALIDATION: compile-pass
// N3485 focus: 14.7.2 [temp.explicit]

template<class T>
T explicit_free(T v)
{
  return v;
}

template int explicit_free<int>(int);

int main()
{
  return 0;
}
