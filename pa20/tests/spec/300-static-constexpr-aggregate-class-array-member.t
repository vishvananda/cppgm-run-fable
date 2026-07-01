// VALIDATION: compile-pass
// N3485 focus: 7.1.5 [dcl.constexpr], 9.4.2 [class.static.data]

template<bool enabled>
struct tables
{
  struct entry
  {
    unsigned short first;
    unsigned short second;
  };

  static constexpr entry data[] = {{0, 0}, {171, 244}, {27324, 0}};
};

template<bool enabled>
constexpr typename tables<enabled>::entry tables<enabled>::data[];

int main()
{
  return tables<true>::data[1].second == 244 ? 0 : 1;
}
