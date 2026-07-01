// VALIDATION: compile-pass
// Taking the address of an lvalue comma expression must evaluate the left side
// for effects and use the right side as the selected storage.

int side;

int &pick(int *values, int index)
{
  return (side = index, values[index]);
}

int main()
{
  int values[2];
  values[0] = 3;
  values[1] = 5;
  pick(values, 1) = 9;
  return values[0] == 3 && values[1] == 9 && side == 1 ? 0 : 1;
}
