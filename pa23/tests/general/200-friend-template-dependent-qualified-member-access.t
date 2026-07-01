// VALIDATION: compile-pass
// A friend function-template declaration can use a renamed template parameter
// inside a dependent qualified member type and still refer to the existing
// namespace function template entity.

template<class C>
struct traits {
  typedef int size_type;
};

template<class C>
class Iter;

template<class C>
void use(Iter<C> it, typename traits<C>::size_type n)
{
  typedef typename Iter<C>::storage_type storage_type;
  storage_type value = n;
  (void)value;
  (void)it;
}

template<class C>
class Iter {
private:
  typedef int storage_type;

public:
  Iter() {}

  template<class D>
  friend void use(Iter<D>, typename traits<D>::size_type);
};

struct Vec {};

int main()
{
  Iter<Vec> it;
  use(it, 3);
  return 0;
}
