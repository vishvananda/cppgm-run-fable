#if !__has_builtin(__builtin_invoke)
#error "__builtin_invoke must be exposed for hosted header compatibility"
#endif

struct Obj {
  int value;
  int add(int x) const { return value + x; }
};

struct Fun {
  int operator()(int x) const { return x + 1; }
};

int invoke_function_object(int x) {
  Fun f;
  return __builtin_invoke(f, x);
}

int invoke_member_function(Obj &obj, int x) {
  return __builtin_invoke(&Obj::add, obj, x);
}

int invoke_member_data(Obj *obj) {
  return __builtin_invoke(&Obj::value, obj);
}

int main() {
  Obj obj = {4};
  int total = invoke_function_object(1) +
              invoke_member_function(obj, 3) +
              invoke_member_data(&obj);
  return total == 13 ? 0 : 1;
}
