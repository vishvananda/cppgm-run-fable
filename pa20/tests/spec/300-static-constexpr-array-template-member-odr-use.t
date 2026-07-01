// VALIDATION: compile-pass
// N3485 focus: 14.5.1 [temp.class], 9.4.2 [class.static.data]

template<class T>
struct tables
{
  static constexpr int data[][2] = {{12 / 3, 5 * 2}, {9 - 6, 1 + 3}};
};

template<class T>
constexpr int tables<T>::data[][2];

struct reader : tables<void>
{
  int read()
  {
    return data[1][0];
  }
};

int main()
{
  reader r;
  return r.read() - 3;
}
