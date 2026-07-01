struct P { long long sig; char opaque[56]; };
struct M {
  P m = {0x32AAABA7LL, {0}};
  M() = default;
};
static_assert(__is_nothrow_constructible(M), "");
int main() { return 0; }
