// VALIDATION: compile-pass
// N3485 focus: 13.5 [over.oper] overloaded logical operators

struct Logic
{
  int value;
};

bool operator&&(const Logic & lhs, const Logic & rhs)
{
  return lhs.value + rhs.value == 3;
}

bool operator||(const Logic & lhs, const Logic & rhs)
{
  return lhs.value + rhs.value == 4;
}

int main()
{
  Logic one;
  Logic two;
  Logic three;
  one.value = 1;
  two.value = 2;
  three.value = 3;
  return (one && two) && (one || three) ? 0 : 1;
}
