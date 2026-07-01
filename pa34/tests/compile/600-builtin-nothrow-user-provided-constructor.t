struct M { M() {} };
static_assert(!__is_nothrow_constructible(M), "");
int main() { return 0; }
