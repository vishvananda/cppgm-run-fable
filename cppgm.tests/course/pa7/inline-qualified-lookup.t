namespace X {
  inline namespace Y {
    inline namespace Z { typedef long TZ; extern int deep; }
    typedef short TY;
  }
}
X::TZ a;
X::TY b;
int X::deep;
typedef X::Z::TZ TT;
TT c;
