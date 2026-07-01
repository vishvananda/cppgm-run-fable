// VALIDATION: compile-pass
// N3485 focus: 14.8.2.1 [temp.deduct.call], 14.7.3 [temp.expl.spec]

namespace lib {
inline namespace v1 {

template<typename T>
struct char_traits;

template<>
struct char_traits<char>;

template<>
struct char_traits<char16_t>;

template<>
struct char_traits<char32_t>;

template<typename T>
struct allocator
{
};

template<typename T,
         typename Traits = char_traits<T>,
         typename Alloc = allocator<T> >
class basic_string;

typedef basic_string<char> string;

template<typename T, typename Traits, typename Alloc>
class basic_string;

template<typename T, typename Traits, typename Alloc>
class basic_string
{
};

}
}

namespace boostish {

template<typename CharT>
class basic_cstring
{
};

template<typename CharT1, typename CharT2>
void assign_op(lib::basic_string<CharT1>&, basic_cstring<CharT2>, int)
{
}

struct begin
{
  basic_cstring<const char> m_file_name;
};

struct log_type
{
  void write(begin const&);
};

lib::string file_name;

void log_type::write(begin const& b)
{
  assign_op(file_name, b.m_file_name, 0);
}

}

int main()
{
  return 0;
}
