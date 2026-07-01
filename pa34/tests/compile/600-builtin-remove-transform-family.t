template<class T>
using remove_const_t = __remove_const(T);

template<class T>
using remove_volatile_t = __remove_volatile(T);

template<class T>
using remove_pointer_t = __remove_pointer(T);

template<class T, class U>
struct same {
  static constexpr bool value = __is_same(T, U);
};

static_assert(same<remove_const_t<const int>, int>::value, "");
static_assert(same<remove_volatile_t<volatile long>, long>::value, "");
static_assert(same<remove_pointer_t<int*>, int>::value, "");

int main() {
  return 0;
}
