// VALIDATION: compile-pass
// N3485 focus: 14.5.2 [temp.mem]

template<bool B, class T = void>
struct enable_if
{
};

template<class T>
struct enable_if<true, T>
{
  typedef T type;
};

template<bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

struct input_it
{
};

struct bidi_it
{
};

template<class T>
struct is_input
{
  static const bool value = false;
};

template<>
struct is_input<input_it>
{
  static const bool value = true;
};

template<class T>
struct is_bidi
{
  static const bool value = false;
};

template<>
struct is_bidi<bidi_it>
{
  static const bool value = true;
};

template<class T>
struct container
{
  template<class It, enable_if_t<is_input<It>::value, int> = 0>
  int insert(It, It);

  template<class It, enable_if_t<is_bidi<It>::value, int> = 0>
  int insert(It, It);
};

template<class T>
template<class It, enable_if_t<is_input<It>::value, int> >
int container<T>::insert(It, It)
{
  return 1;
}

template<class T>
template<class It, enable_if_t<is_bidi<It>::value, int> >
int container<T>::insert(It, It)
{
  return 2;
}

int main()
{
  container<int> c;
  bidi_it first;
  bidi_it last;
  return c.insert(first, last) == 2 ? 0 : 1;
}
