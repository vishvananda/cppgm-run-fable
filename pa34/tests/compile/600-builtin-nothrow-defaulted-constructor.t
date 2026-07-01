struct M { M() = default; };
static_assert(__is_nothrow_constructible(M), "");
int main() { return 0; }
