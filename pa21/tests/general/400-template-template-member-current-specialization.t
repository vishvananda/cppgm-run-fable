// VALIDATION: compile-pass
// A template-template argument whose owner uses the current specialization must
// remain the same resolved template when replayed through a member call.

template<class Self, class T, class Alloc>
struct LayoutImpl
{
};

template<class A, class B, template<class, class, class> class Layout>
struct Box
{
  void f(Box<A, B, Layout> &)
  {
  }
};

Box<int, int, LayoutImpl> a;
Box<int, int, LayoutImpl> b;

int main()
{
  a.f(b);
  return 0;
}
