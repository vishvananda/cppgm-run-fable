template<typename T>
struct InsertBase {
  typedef unsigned long size_type;

  void insert(int) {}
  void insert(int, int) {}

  template<typename It>
  void insert(It, It) {}
};

template<typename T, bool Flag>
struct Insert;

template<typename T>
struct Insert<T, false> : InsertBase<T> {
  typedef InsertBase<T> base_type;
  using typename base_type::size_type;
  using base_type::insert;

  void insert(long) {}
  void insert(int, long) {}
};

template<typename T>
struct Table : Insert<T, false> {
  typedef Insert<T, false> base_type;
  typedef typename base_type::size_type size_type;

  template<typename It>
  Table(It first, It last, size_type hint);
};

template<typename T>
template<typename It>
Table<T>::Table(It first, It last, size_type hint) {
  (void)hint;
  this->template insert<It>(first, last);
}

int *source();
void consume(Table<int>);

int main() {
  consume(Table<int>(source(), source(), 0));
  return 0;
}
