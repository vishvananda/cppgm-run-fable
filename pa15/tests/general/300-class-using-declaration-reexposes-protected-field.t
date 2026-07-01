// VALIDATION: compile-pass

class using_access_base
{
protected:
  int value;
};

class using_access_property : public using_access_base
{
public:
  using using_access_base::value;
};

int main()
{
  using_access_property p;
  p.value = 5;
  return p.value == 5 ? 0 : 1;
}
