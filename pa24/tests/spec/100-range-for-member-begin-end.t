// VALIDATION: compile-pass
// N3485 focus: 6.5.4 [stmt.ranged]

struct Range
{
  int data[3];

  int * begin() { return data; }
  int * end() { return data + 3; }
};

int main()
{
  Range r = {{1, 2, 3}};
  int sum = 0;
  for(int x : r) {
    sum += x;
  }
  return sum == 6 ? 0 : 1;
}
