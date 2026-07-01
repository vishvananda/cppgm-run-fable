template<class T>
struct Holder {
  T value;
};

Holder<__int128 unsigned> after_named;
Holder<unsigned __int128> before_named;

int main() {
  return sizeof(after_named.value) == sizeof(before_named.value) ? 0 : 1;
}
