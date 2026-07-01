template<bool B>
struct bool_constant {
  static const bool value = B;
};

using fn_type = int();
using fn_ref = int(&)();

template<class T>
struct is_function : bool_constant<__is_function(T)> {};

static_assert(!is_function<int>::value, "");
static_assert(is_function<fn_type>::value, "");
static_assert(!is_function<fn_ref>::value, "");
static_assert(!is_function<int*>::value, "");

int main() {
  return is_function<fn_type>::value ? 0 : 1;
}
