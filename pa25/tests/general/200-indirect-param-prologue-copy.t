struct S {
  S();
  S(const S &);
  S(S &&);
  ~S();
};

struct E {
  S a;
  S b;
  S c;
};

int main()
{
  S x;
  S y;
  S z;
  E e = {x, y, z};
  (void)e;
}
