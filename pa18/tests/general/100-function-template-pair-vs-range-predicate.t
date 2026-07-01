// VALIDATION: compile-pass
// Boost.Algorithm is_palindrome reduction: in a non-dependent function body,
// ordinary lookup must use the overload set visible at the definition, and
// the pair template must beat the range+predicate template.

template<class I>
int choose(I, I)
{
  return 1;
}

template<class R, class P>
int choose(const R &, P)
{
  return 2;
}

int choose(const char * p)
{
  return choose(p, p + 1);
}

template<class P>
int choose(const char *, P)
{
  return 3;
}

int main()
{
  return choose("x") == 1 ? 0 : 1;
}
