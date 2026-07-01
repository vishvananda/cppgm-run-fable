// VALIDATION: compile-pass
// N3485 focus: 14.5.5 [temp.class.spec]; variable templates are post-N3485.

template<class A, class T, class = void>
const bool pair_v = false;

template<class A, class T>
const bool pair_v<A, T*, void> = true;

int main()
{
  return pair_v<int, int*> ? 0 : 1;
}
