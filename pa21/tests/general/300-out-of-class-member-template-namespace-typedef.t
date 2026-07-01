namespace N {
typedef int count_t;

template<class T>
struct Holder {
  template<class... Args>
  int make(count_t h, Args... args);
};

template<class T>
template<class... Args>
int Holder<T>::make(count_t h, Args... args) {
  return h + sizeof...(Args);
}
}

int main() {
  N::Holder<int> *h = 0;
  return h->template make<int, char>(3, 0, 'x') == 5 ? 0 : 1;
}
