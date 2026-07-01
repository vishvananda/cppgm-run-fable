// VALIDATION: compile-pass
// N3485 focus: 5.19 [expr.const], 7.1.5 [dcl.constexpr], 14.3.2 [temp.arg.nontype]

struct skip_info
{
  unsigned int size_at_begin;
};

constexpr skip_info skip()
{
  return skip_info{1};
}

constexpr unsigned int member_value = skip().size_at_begin;
static_assert(member_value == 1, "constexpr aggregate member access should evaluate");

template<unsigned int N>
constexpr unsigned int value()
{
  return N;
}

static_assert(value<3 - skip().size_at_begin>() == 2,
              "constexpr aggregate member access should work in non-type template arguments");

int main()
{
  return value<3 - skip().size_at_begin>() == 2 ? 0 : 1;
}
