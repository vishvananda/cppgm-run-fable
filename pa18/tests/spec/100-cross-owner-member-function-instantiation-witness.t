// VALIDATION: compile-pass
// N3485 focus: 14.7.1 [temp.inst]
// Instantiating a function template body may instantiate an ordinary member
// function from another class template owner. The witness closure should expose
// that member instantiation without an extra public ensure-definition step.

template<class T>
struct box
{
  int touch()
  {
    return 1;
  }
};

template<class T>
int call_touch(box<T> & value)
{
  return value.touch();
}

int main()
{
  box<int> value;
  return call_touch(value) - 1;
}
