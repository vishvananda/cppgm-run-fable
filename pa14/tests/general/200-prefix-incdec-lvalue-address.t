// VALIDATION: compile-pass
// Built-in prefix increment and decrement produce an lvalue designating the
// original operand storage, so taking the address of the prefix expression
// must produce the address of the pointer variable rather than its stored
// pointer value.

int main()
{
  int values[3];
  int *p = values;
  int **paddr = &p;
  int ok_inc = &(++p) == paddr;

  int *q = values + 2;
  int **qaddr = &q;
  int ok_dec = &(--q) == qaddr;

  return ok_inc && ok_dec ? 0 : 1;
}
