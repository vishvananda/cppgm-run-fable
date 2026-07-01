// VALIDATION: compile-pass

template<class Parser, class Action>
struct parser_action
{
  int value;
  parser_action(Parser const &, Action const &) : value(7) {}
};

template<class Derived>
struct parser_base
{
  template<class Action>
  parser_action<Derived, Action> operator[](Action const & action) const
  {
    return parser_action<Derived, Action>(static_cast<Derived const &>(*this),
                                          action);
  }
};

struct action
{
};

struct rule : parser_base<rule>
{
  int stored;

  rule() : stored(0) {}

  template<class ParserAction>
  rule & operator=(ParserAction const & parser)
  {
    stored = parser.value;
    return *this;
  }
};

int main()
{
  rule name;
  rule tag;
  tag = name[action()];
  return tag.stored == 7 ? 0 : 1;
}
