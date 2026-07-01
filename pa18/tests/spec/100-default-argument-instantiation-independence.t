// VALIDATION: compile-pass
// N3485 focus: 14.6.4.1 [temp.point], 14.7.1 [temp.inst]

template<typename T>
T ydef(T)
{
  return T(40);
}

template<typename T>
T zdef(T)
{
  return T(2);
}

template<typename T>
T sum(T x, T y = ydef(T()), T z = zdef(T()))
{
  return x + y + z;
}

int main()
{
  return sum(1, 3) == 6 && sum(1, 3, 4) == 8 ? 0 : 1;
}
