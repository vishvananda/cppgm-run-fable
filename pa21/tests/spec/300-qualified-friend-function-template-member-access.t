// VALIDATION: compile-pass
// N3485 focus: 14.5.4 [temp.friend]

namespace N {

template<class T>
class Box;

namespace detail {
template<class T>
int read(Box<T> box);
}

template<class T>
class Box {
private:
  int secret;
public:
  Box(int value) : secret(value) {}

  template<class U>
  friend int detail::read(Box<U>);
};

namespace detail {
template<class U>
int read(Box<U> box)
{
  return box.secret;
}
}

}

int main()
{
  return N::detail::read(N::Box<int>(7)) == 7 ? 0 : 1;
}
