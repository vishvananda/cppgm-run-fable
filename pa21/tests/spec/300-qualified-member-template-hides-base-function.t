// VALIDATION: compile-pass
// N3485 focus: 10.2 [class.member.lookup], 14.5.2 [temp.mem]

struct root
{
  int insert_(int, int & value, int)
  {
    value = 1;
    return 1;
  }
};

struct ordered : root
{
  typedef root super;

  template<class Variant>
  int insert_(int, int & value, Variant variant)
  {
    value = 7;
    return super::insert_(0, value, variant) + 1;
  }
};

struct container : ordered
{
  typedef ordered super;

  int insert(int & value)
  {
    return super::insert_(0, value, 0);
  }
};

int main()
{
  int value = 0;
  container c;
  int result = c.insert(value);
  return result == 2 && value == 1 ? 0 : 1;
}
