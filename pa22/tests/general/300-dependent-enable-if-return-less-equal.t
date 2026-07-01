namespace std {
typedef unsigned long size_t;

template<class T>
struct iterator_traits {
  typedef typename T::value_type value_type;
};

template<bool B, class T = void>
struct enable_if {};

template<class T>
struct enable_if<true, T> {
  typedef T type;
};

template<class T>
struct numeric_limits {
  static const int digits = 32;
};
}

template<class T>
struct is_char_type {
  static const bool value = false;
};

template<>
struct is_char_type<char> {
  static const bool value = true;
};

template<class It>
typename std::enable_if<
    is_char_type<typename std::iterator_traits<It>::value_type>::value &&
        std::numeric_limits<std::size_t>::digits <= 32,
    std::size_t>::type
hash_range(std::size_t seed, It, It)
{
  return seed;
}

struct iter {
  typedef char value_type;
};

int main()
{
  iter first;
  iter last;
  return hash_range(0, first, last);
}
