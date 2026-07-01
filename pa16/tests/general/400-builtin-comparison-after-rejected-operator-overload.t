// VALIDATION: compile-pass

struct Property
{
  typedef unsigned const& read_access_t;

  unsigned value;

  Property() : value(1) {}

  operator read_access_t() const
  {
    return value;
  }
};

struct Other {};

bool operator!=(Property const&, Other const&);

enum masks
{
  off = 0
};

int main()
{
  Property p;
  return p != off ? 0 : 1;
}
