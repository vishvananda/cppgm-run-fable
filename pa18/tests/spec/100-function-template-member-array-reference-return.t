// VALIDATION: compile-pass
// N3485 focus: 8.3.5 [dcl.fct], 14.8.2.1 [temp.deduct.call]

template<class T>
struct holder
{
  T elems[2];
};

template<class T>
T (&get_elems(holder<T>& value))[2]
{
  return value.elems;
}

int main()
{
  holder<int> value;
  value.elems[0] = 7;
  int (&ref)[2] = get_elems(value);
  ref[1] = 5;
  return value.elems[0] == 7 && value.elems[1] == 5 ? 0 : 1;
}
