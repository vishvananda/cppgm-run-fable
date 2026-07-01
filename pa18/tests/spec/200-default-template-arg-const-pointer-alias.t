// VALIDATION: compile-pass
// N3485 focus: 14.1 [temp.param], 14.3.1 [temp.arg.type]

template<class T>
struct wrap {};

template<class Key, class Alloc = wrap<const Key> >
struct box {};

struct node {};

int accept(box<const node *, wrap<const node * const> >)
{
  return 0;
}

int main()
{
  box<const node *> x;
  return accept(x);
}
