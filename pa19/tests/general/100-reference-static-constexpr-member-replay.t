template<int Size = 8, int Align = 8>
struct basic_any {
  static constexpr int buffer_size = Size;
  static constexpr int buffer_align = Align;

  basic_any() {}
};

template<class Any>
struct basic_tests {
  struct copy_counter {
    copy_counter() {}
    copy_counter(const copy_counter &) { ++count; }

    static int get_count()
    {
      return count;
    }

    static int count;
  };

  static int run_tests()
  {
    Any value;
    copy_counter first;
    copy_counter second = first;
    int observed = copy_counter::get_count();
    return observed + value.buffer_size + value.buffer_align;
  }
};

template<class Any>
int basic_tests<Any>::copy_counter::count = 0;

int main()
{
  const int result = basic_tests<basic_any<> >::run_tests();
  return result - 17;
}
