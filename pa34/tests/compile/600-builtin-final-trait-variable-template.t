struct NonFinal {};
struct Final final {};

template<class T>
inline constexpr bool final_v = __is_final(T);

static_assert(!__is_final(NonFinal), "");
static_assert(__is_final(Final), "");
static_assert(!final_v<NonFinal>, "");
static_assert(final_v<Final>, "");

int main() {
  return 0;
}
