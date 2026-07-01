template<typename T>
struct box {
  template<typename U>
  static int cast(U u);
};

template<typename T>
template<typename U>
int box<T>::cast(U u) {
  return static_cast<int>(sizeof(T)) + static_cast<int>(u);
}

int main() {
  return box<int>::cast(3) == static_cast<int>(sizeof(int)) + 3 ? 0 : 1;
}
