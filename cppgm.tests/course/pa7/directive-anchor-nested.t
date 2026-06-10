namespace A2 {
  typedef long i;
  namespace B {
    namespace C { typedef int i; }
    using namespace A2::B::C;
    i x;
  }
}
