template<class T, T V>
struct integral_constant {
  static const T value = V;
  constexpr operator T() const { return value; }
};

typedef integral_constant<bool, true> true_type;
typedef integral_constant<bool, false> false_type;

template<bool B>
struct box {
  static const bool value = B;
};

template<class B>
struct use_member_value {
  typedef box<bool(B::value)> type;
};

template<class B>
struct use_constant_object {
  typedef box<B{}> type;
};

static_assert(use_member_value<true_type>::type::value, "");
static_assert(!use_member_value<false_type>::type::value, "");
static_assert(use_constant_object<true_type>::type::value, "");

int main()
{
  return 0;
}
