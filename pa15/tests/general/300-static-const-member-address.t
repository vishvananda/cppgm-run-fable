// VALIDATION: compile-pass

struct static_const_member_address_holder
{
  static const char null;

  static const char * get()
  {
    return &null;
  }
};

const char static_const_member_address_holder::null = 0;

int main()
{
  return *static_const_member_address_holder::get();
}
