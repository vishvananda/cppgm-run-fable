namespace std {
class type_info;
}

struct LambdaRttiHost {
  const void *capture(int base)
  {
    auto add = [base](int delta) { return base + delta; };
    return &typeid(add);
  }
};

int main()
{
  LambdaRttiHost value;
  return value.capture(7) ? 0 : 1;
}
