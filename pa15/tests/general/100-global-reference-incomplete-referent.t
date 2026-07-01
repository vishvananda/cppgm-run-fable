// VALIDATION: compile-pass

namespace std
{
  struct allocator_arg_t;
}

std::allocator_arg_t *forward_declared_object;
typedef const std::allocator_arg_t & allocator_arg_ref;
static allocator_arg_ref allocator_arg = *forward_declared_object;

int value = 7;
int &value_ref = value;

int main()
{
  value_ref = 9;
  return value == 9 ? 0 : 1;
}
