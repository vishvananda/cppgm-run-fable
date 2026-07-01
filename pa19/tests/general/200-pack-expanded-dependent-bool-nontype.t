template<class T, T V>
struct integral_constant {
  static const T value = V;
};

typedef integral_constant<bool, true> true_type;
typedef integral_constant<bool, false> false_type;

template<class... T>
struct list {
};

template<class... Bn>
struct values {
  typedef list<integral_constant<bool, bool(Bn::value)>...> type;
};

values<true_type, false_type, true_type>::type *result;

int main()
{
  return result == 0 ? 0 : 1;
}
