namespace std
{
  template<class _Tp>
  struct __is_integer
  {
    static const bool __value = false;
  };
}

// Importing a base member VALUE whose qualifier is spelled with the class
// template's own parameter; on instantiation the qualifier resolves to a
// concrete base (std::__is_integer<long>) and the using-declaration must bind
// its __value. (Mirrors libstdc++'s __gnu_cxx::__is_integer_nonstrict.)
template<class _Tp>
struct __is_integer_nonstrict : std::__is_integer<_Tp>
{
  using std::__is_integer<_Tp>::__value;
};

static_assert(!__is_integer_nonstrict<long>::__value,
              "using-declaration imports the concrete base member value");

int main()
{
  return 0;
}
