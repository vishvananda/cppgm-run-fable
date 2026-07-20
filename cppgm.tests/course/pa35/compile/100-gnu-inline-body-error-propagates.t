// PA35 audit: a gnu_inline wrapper body with a genuine semantic error
// (an undeclared non-builtin name) must fail the compile, not silently
// demote to a declaration.
extern __inline __attribute__((__gnu_inline__)) int broken_wrapper(void)
{
	return nonexistent_identifier;
}
int main() { return 0; }
