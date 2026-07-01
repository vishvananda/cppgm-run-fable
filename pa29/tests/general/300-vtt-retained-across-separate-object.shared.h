struct V {
  int v;
  V();
  virtual ~V();
};

struct A : virtual V {
  int a;
  A();
  A(const A &);
  virtual ~A();
  virtual int f();
};

struct X : virtual A {
  int x;
  X();
  X(const X &);
  ~X();
  int f();
};
