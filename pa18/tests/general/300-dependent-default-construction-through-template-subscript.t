// VALIDATION: compile-pass
// N3485 focus: core-language reduction of map subscript insertion surface

struct value_box
{
  int value;

  value_box() noexcept : value(0) {}
};

template<typename Key, typename Value>
struct tiny_assoc
{
  Key stored_key;
  Value stored_value;
  bool occupied;

  tiny_assoc() noexcept : stored_key(), stored_value(), occupied(false) {}

  Value & subscript(const Key & key) noexcept
  {
    if(!occupied || stored_key != key) {
      stored_key = key;
      stored_value = Value();
      occupied = true;
    }
    return stored_value;
  }
};

int main()
{
  tiny_assoc<unsigned char, value_box> values;
  unsigned char key = 1;
  values.subscript(key).value = 3;
  return values.subscript(key).value == 3 ? 0 : 1;
}
