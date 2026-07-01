// VALIDATION: compile-pass
// N3485 focus: 14.7.3 [temp.expl.spec]

template<class T>
class complex;

template<>
class complex<float> {
  float re_;

public:
  explicit complex(const complex<double> & c);
  float real() const { return re_; }
};

template<>
class complex<double> {
  double re_;

public:
  explicit complex(double re = 0) : re_(re) {}
  double real() const { return re_; }
};

inline complex<float>::complex(const complex<double> & c) : re_(c.real()) {}

int main()
{
  complex<double> d(1.0);
  complex<float> f(d);
  return f.real() == 1.0f ? 0 : 1;
}
