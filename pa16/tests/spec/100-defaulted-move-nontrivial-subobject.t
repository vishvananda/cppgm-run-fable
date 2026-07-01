// VALIDATION: compile-pass
// N3485 focus: 8.4.2 [dcl.fct.def.default], 12.8 [class.copy]

struct Stateful
{
  Stateful(int state_value = 0): state(state_value) {}
  Stateful(const Stateful&) = default;
  Stateful(Stateful&& other): state(other.state) { other.state = -1; }

  int state;
};

struct BaseHolder: Stateful
{
  BaseHolder(): Stateful(42) {}
  BaseHolder(BaseHolder&&) = default;
};

struct MemberHolder
{
  MemberHolder(): member(17) {}
  MemberHolder(MemberHolder&&) = default;

  Stateful member;
};

int main()
{
  BaseHolder base_source;
  BaseHolder base_dest(static_cast<BaseHolder&&>(base_source));

  MemberHolder member_source;
  MemberHolder member_dest(static_cast<MemberHolder&&>(member_source));

  return base_source.state == -1 &&
         base_dest.state == 42 &&
         member_source.member.state == -1 &&
         member_dest.member.state == 17 ? 0 : 1;
}
