// VALIDATION: compile-pass
// N3485 focus: 14.2 [temp.names], 3.4.3 [basic.lookup.qual]

namespace api {
template<class T>
struct function
{
  char value;
};
}

struct Value {};

int main()
{
  int function = 0;
  api::function<void(const Value &)> callback;
  (void)callback;
  return function;
}
