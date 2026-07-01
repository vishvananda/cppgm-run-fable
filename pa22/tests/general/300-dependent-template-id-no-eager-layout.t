namespace N {

template<typename T>
struct Box {
  using value_type = T;
  T value;
};

template<typename T>
using diff_t = typename Box<T>::difference_type;

template<typename I>
inline I fetch_add_explicit(Box<I> * box, diff_t<I> offset, int mode) noexcept
{
  return box->fetch_add(offset, mode);
}

}

int main()
{
  return 0;
}
