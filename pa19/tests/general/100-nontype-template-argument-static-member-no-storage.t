template<class T, T V>
struct value_source {
  static const T const_min = V;
};

template<class T, T V>
const T value_source<T, V>::const_min;

struct traits : value_source<int, -100> {
};

template<bool B>
struct box {
};

template<int N>
struct use_value {
  typedef box<(N >= traits::const_min)> type;
};

use_value<-50>::type *result;

int main()
{
  return result == 0 ? 0 : 1;
}
