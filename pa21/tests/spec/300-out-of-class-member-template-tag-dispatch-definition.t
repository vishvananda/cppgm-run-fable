struct input_tag {};
struct forward_tag : input_tag {};

template<class T>
struct box {
  template<class It>
  int pick(It, It, input_tag);

  template<class It>
  int pick(It, It, forward_tag);
};

template<class T>
template<class It>
int box<T>::pick(It, It, input_tag) {
  return 1;
}

template<class T>
template<class It>
int box<T>::pick(It, It, forward_tag) {
  return 2;
}

char *source();
forward_tag make_forward_tag();

int main() {
  box<int> *b = 0;
  return b->pick(source(), source(), make_forward_tag()) == 2 ? 0 : 1;
}
