template<class T>
T&& declval();

template<class, class A, class... Args>
inline const bool has_make_impl = false;

template<class A, class... Args>
inline const bool has_make_impl<
    decltype((void)declval<A>().make(declval<Args>()...)), A, Args...> = true;

template<class A, class... Args>
inline const bool has_make_v = has_make_impl<void, A, Args...>;

struct maker {
  void make(int*, int) {}
};

int main()
{
  return has_make_v<maker, int*, int> ? 0 : 1;
}
