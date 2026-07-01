// VALIDATION: compile-fail
// N3485 focus: 14.7.3 [temp.expl.spec]

template<class T>
struct box
{
  box();
};

template<>
struct box<int>
{
  box();
};

template<>
box<int>::box() {}
