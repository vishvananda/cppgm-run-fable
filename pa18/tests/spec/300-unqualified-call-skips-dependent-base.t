// VALIDATION: compile-pass
// Boost.Config BOOST_NO_TWO_PHASE_NAME_LOOKUP reduction.
// N3485 focus: 14.6.2 [temp.dep], 14.6.3 [temp.nondep]

template<class T>
struct two_phase_base
{
  int call()
  {
    return 1;
  }
};

int call()
{
  return 0;
}

template<class T>
struct two_phase_derived : two_phase_base<T>
{
  int run()
  {
    return call();
  }
};

int main()
{
  two_phase_derived<int> d;
  return d.run();
}
