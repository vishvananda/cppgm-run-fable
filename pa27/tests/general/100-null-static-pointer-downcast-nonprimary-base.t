// VALIDATION: compile-pass

struct left
{
  int a;
};

struct base
{
  int b;
};

struct derived : left, base
{
  int c;
};

int main()
{
  base * p = 0;
  derived * q = static_cast<derived *>(p);
  return q == 0 ? 0 : 1;
}
