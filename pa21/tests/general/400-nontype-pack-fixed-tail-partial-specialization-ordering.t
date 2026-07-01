// VALIDATION: compile-pass
// A fixed non-type prefix should be preferred over a catch-all trailing pack.

template<unsigned char... Bytes>
struct bytes
{
};

template<typename Bytes, char... Chars>
struct bin_literal;

template<unsigned char... Bytes>
struct bin_literal<bytes<Bytes...> >
{
  static const int size = sizeof...(Bytes);
};

template<unsigned char... Bytes, char Bit7, char Bit6, char Bit5,
         char Bit4, char Bit3, char Bit2, char Bit1, char Bit0,
         char... Chars>
struct bin_literal<bytes<Bytes...>, Bit7, Bit6, Bit5, Bit4,
                   Bit3, Bit2, Bit1, Bit0, Chars...>
  : bin_literal<bytes<Bytes..., 7>, Chars...>
{
};

template<unsigned char... Bytes, char... Chars>
struct bin_literal<bytes<Bytes...>, Chars...>
{
  static const int size = -1;
};

int main()
{
  return bin_literal<bytes<>, '0', '0', '0', '0',
                     '0', '0', '0', '0'>::size == 1 ? 0 : 1;
}
