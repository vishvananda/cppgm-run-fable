template<class Ch, class Tr, class A>
struct basic_string;

struct char_traits {};
struct allocator {};

template<class Ch>
struct basic_string_view {
  Ch const* data() const { return "abc"; }
  unsigned long size() const { return 3; }

  template<class A>
  operator basic_string<Ch, char_traits, A>() const;
};

template<class Ch, class Tr, class A>
struct basic_string {
  explicit basic_string(basic_string_view<Ch> const&) {}
  basic_string(Ch const*, unsigned long) {}
};

template<class Ch>
template<class A>
basic_string_view<Ch>::operator basic_string<Ch, char_traits, A>() const {
  return basic_string<Ch, char_traits, A>(data(), size());
}

basic_string_view<char> make_view()
{
  return basic_string_view<char>();
}

int main()
{
  basic_string<char, char_traits, allocator> value = make_view();
  (void)value;
  return 0;
}
