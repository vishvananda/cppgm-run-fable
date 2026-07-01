// VALIDATION: compile-pass
// N3485 focus: 14.7.2 [temp.explicit]

template<class T>
struct box
{
  box();
};

extern template box<int>::box();

int main()
{
  return 0;
}
