// VALIDATION: compile-pass
// N3485 focus: 5.2.2 [expr.call], 14.8.2.1 [temp.deduct.call]

struct String {};

namespace boost
{
  namespace exception_detail
  {
    struct error_info_container
    {
      virtual char const * diagnostic_information(char const *) const = 0;
    };

    struct impl : error_info_container
    {
      char const * diagnostic_information(char const *) const
      {
        return "ok";
      }
    };

    char const * get_diagnostic_information(error_info_container * c, char const * header)
    {
      char const * di = c->diagnostic_information(header);
      return di;
    }
  }

  template<class T>
  String diagnostic_information(T const &, bool = true);
}

int main()
{
  boost::exception_detail::impl c;
  return boost::exception_detail::get_diagnostic_information(&c, 0) ? 0 : 1;
}
