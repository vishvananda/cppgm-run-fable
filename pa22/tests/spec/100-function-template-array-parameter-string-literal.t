// VALIDATION: compile-pass
// N3485 focus: 14.8.2.1 [temp.deduct.call]

template<class T>
struct char_traits;

template<>
struct char_traits<char>
{
  typedef char char_type;
};

template<class A, class B>
struct is_same
{
  static const bool value = false;
};

template<class A>
struct is_same<A, A>
{
  static const bool value = true;
};

template<class CharT, class Traits = char_traits<CharT> >
struct string_like
{
  typedef Traits traits_type;
  static_assert(is_same<CharT, typename traits_type::char_type>::value, "");
};

template<class Char, class Traits = char_traits<Char> >
struct delimiter_type
{
  typedef string_like<Char, Traits> string_type;
  string_type data;
  delimiter_type(string_type const& d) : data(d) {}
};

template<class Char>
delimiter_type<Char> delimiter(Char const s[])
{
  (void)s;
  return delimiter_type<Char>(string_like<Char>());
}

template<class Char>
delimiter_type<Char> delimiter(Char c)
{
  (void)c;
  return delimiter_type<Char>(string_like<Char>());
}

int main()
{
  delimiter(",");
  return 0;
}
