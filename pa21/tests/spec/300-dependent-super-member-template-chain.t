// VALIDATION: compile-pass
// N3485 focus: 10.2 [class.member.lookup], 14.5.2 [temp.mem]

struct tag
{
};

struct terminal
{
  int insert_(int, int * position, int & value, tag)
  {
    value = position == 0 ? 3 : 4;
    return value;
  }
};

template<class Super>
struct layer : Super
{
  typedef Super super;

  template<class Variant>
  int insert_(int, int * position, int & value, Variant variant)
  {
    value += 100;
    return super::insert_(0, position, value, variant) + 100;
  }
};

struct second : layer<terminal>
{
};

struct first : layer<second>
{
};

int main()
{
  first f;
  int value = 0;
  int * position = 0;
  int result = f.insert_(0, position, value, tag());
  return result == 203 && value == 3 ? 0 : 1;
}
