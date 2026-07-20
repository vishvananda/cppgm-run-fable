// PA35: a gnu_inline hosted-intrinsic wrapper whose body calls a
// target builtin outside the implemented surface demotes to a
// declaration; the translation unit still compiles.
extern __inline __attribute__((__gnu_inline__, __always_inline__)) void
wrapper_emms(void)
{
	__builtin_ia32_emms();
}
static_assert(sizeof(void (*)(void)) == 8, "anchor");
int main() { return 0; }
