template<unsigned char... Bytes>
struct bytes {
};

template<class T>
struct literal;

template<unsigned char... Bytes>
struct literal<bytes<Bytes...> > {
  static const unsigned char data[sizeof...(Bytes)];
};

template<unsigned char... Bytes>
const unsigned char literal<bytes<Bytes...> >::data[sizeof...(Bytes)] = {
  Bytes...
};

int main()
{
  return literal<bytes<4, 7> >::data[1] - 7;
}
