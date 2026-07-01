__int128 unsigned direct_value;

typedef __int128 unsigned uint128_after_named;
typedef unsigned __int128 uint128_before_named;

static_assert(sizeof(uint128_after_named) == 16, "postfix unsigned int128 size");
static_assert(sizeof(uint128_before_named) == 16, "prefix unsigned int128 size");

uint128_after_named convert(uint128_before_named value) {
  return (uint128_after_named)value;
}

int main() {
  return sizeof(direct_value) == 16 ? 0 : 1;
}
