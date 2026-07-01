// VALIDATION: compile-pass
// N3485 focus: 14.5.2 [temp.mem], 7.1.6.2 [dcl.type.simple]

struct table
{
  int call(int * value) const
  {
    return *value + 1;
  }
};

struct wrapper
{
  table * fn_table_;
  int * impl_;

  template<typename T>
  auto call_it() -> decltype(fn_table_->call(impl_))
  {
    return fn_table_->call(impl_);
  }
};

int main()
{
  table table_value;
  int value = 4;
  wrapper w;
  w.fn_table_ = &table_value;
  w.impl_ = &value;
  return w.call_it<int>() == 5 ? 0 : 1;
}
