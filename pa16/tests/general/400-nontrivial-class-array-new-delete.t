static int constructed;
static int destroyed;
static int fail;
static int expected = 3;

struct Item
{
  int value;

  Item() : value(++constructed) {}
  ~Item()
  {
    if(value != expected) {
      fail = 1;
    }
    --expected;
    destroyed += value;
  }
};

int main()
{
  Item *items = new Item[3];
  int ok = constructed == 3 && items[0].value == 1 && items[2].value == 3;
  delete[] items;
  return ok && !fail && destroyed == 6 && expected == 0 ? 0 : 1;
}
// VALIDATION: compile-pass
