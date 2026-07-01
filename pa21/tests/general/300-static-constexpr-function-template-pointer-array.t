// VALIDATION: compile-pass
// N3485 focus: 14.3.2 [temp.arg.nontype], 9.4.2 [class.static.data]

typedef unsigned long size_t;
typedef unsigned long long uint64_t;

template<class = void>
struct table
{
  constexpr static size_t sizes[] = {13ul, 29ul, 53ul};
  constexpr static uint64_t inv_sizes32[] = {1ull, 2ull};
  constexpr static size_t inv_sizes32_len =
      sizeof(inv_sizes32) / sizeof(inv_sizes32[0]);

  template<size_t Index, size_t Size = sizes[Index]>
  static size_t position(size_t hash)
  {
    return hash % Size;
  }

  constexpr static size_t (*positions[])(size_t) = {
    position<2, sizes[2]>
  };

  static size_t call(size_t hash, size_t index)
  {
    size_t sizes_under_32bit = inv_sizes32_len;
    if(index < sizes_under_32bit) {
      return (hash + inv_sizes32[index]) % sizes[index];
    }
    return positions[index - sizes_under_32bit](hash);
  }
};

template<class T>
constexpr size_t table<T>::sizes[];

template<class T>
constexpr uint64_t table<T>::inv_sizes32[];

template<class T>
constexpr size_t (*table<T>::positions[])(size_t);

int main()
{
  return table<>::call(42ul, 1ul) == 15ul &&
         table<>::call(42ul, 2ul) == 42ul ? 0 : 1;
}
