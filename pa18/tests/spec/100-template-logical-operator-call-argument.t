// VALIDATION: compile-pass
// N3485 focus: 13.5 [over.oper], 14.8.2 [temp.deduct]

struct binder
{
  int value;
};

template<class L, class R>
bool operator&&(const L & lhs, const R & rhs)
{
  return lhs.value == 1 && rhs.value == 0;
}

template<class L, class R>
bool operator||(const L & lhs, const R & rhs)
{
  return lhs.value == 1 || rhs.value == 1;
}

template<class F, class A1, class A2, class R>
int test(F f, A1 a1, A2 a2, R r)
{
  return f == r && a1 == false && a2 == true ? 0 : 1;
}

int main()
{
  binder one;
  binder zero;
  one.value = 1;
  zero.value = 0;
  return test(one && zero, false, true, true) +
         test(zero || one, false, true, true);
}
