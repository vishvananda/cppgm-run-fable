struct pad
{
  int x;
  pad() : x(7) {}
};

template<class Final>
struct holder
{
  int marker;
  holder() : marker(11) {}
  Final * final() { return static_cast<Final *>(this); }
  int read() { return final()->value; }
};

class object : pad, private holder<object>
{
  friend struct holder<object>;
  int value;

public:
  object() : pad(), holder<object>(), value(42) {}
  int run() { return holder<object>::read(); }
};

int main()
{
  object o;
  return o.run() == 42 ? 0 : 1;
}
