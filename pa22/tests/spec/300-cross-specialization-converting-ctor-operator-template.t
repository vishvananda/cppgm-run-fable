// VALIDATION: compile-pass
// N3485 focus: 14.8 [temp.fct.spec], 14.7.3 [temp.expl.spec]

template<bool B, class T = void>
struct enable_if
{
};

template<class T>
struct enable_if<true, T>
{
  typedef T type;
};

template<class A, class B>
struct is_same
{
  static const bool value = false;
};

template<class A>
struct is_same<A, A>
{
  static const bool value = true;
};

template<class T>
class box;

template<class T>
box<T> operator+(const box<T>&, const box<T>&);

struct tag
{
};

template<class T>
class box
{
public:
  T real() const
  {
    return T();
  }

  T imag() const
  {
    return T();
  }
};

template<>
class box<double>;

template<>
class box<float>
{
public:
  template<class U, typename enable_if<is_same<U, tag>::value, int>::type = 0>
  explicit box(U, float);

  explicit box(const box<double>&);

  float real() const
  {
    return 0.0f;
  }

  float imag() const
  {
    return 0.0f;
  }
};

template<>
class box<double>
{
public:
  template<class U, typename enable_if<is_same<U, tag>::value, int>::type = 0>
  explicit box(U, double);

  box(const box<float>&);

  double real() const
  {
    return 0.0;
  }

  double imag() const
  {
    return 0.0;
  }
};

inline box<float>::box(const box<double>&)
{
}

template<class T>
box<T> operator+(const box<T>&, const box<T>&)
{
  return box<T>();
}

int main()
{
  return 0;
}
