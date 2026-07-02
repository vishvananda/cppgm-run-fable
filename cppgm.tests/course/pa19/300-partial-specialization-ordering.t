template<class A, class B>
struct pick { static const int value = 0; };

template<class A, class B2>
struct pick<A*, B2> { static const int value = 1; };

template<class A2, class B>
struct pick<A2*, B*> { static const int value = 2; };

static_assert(pick<int, int>::value == 0, "primary");
static_assert(pick<int*, int>::value == 1, "one");
static_assert(pick<int*, int*>::value == 2, "two");

int main() { return 0; }
