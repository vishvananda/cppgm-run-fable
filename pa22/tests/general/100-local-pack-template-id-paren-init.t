template<typename A, typename B>
struct pair {
  A first;
  B second;
  pair(A a, B b) : first(a), second(b) {}
};

template<typename... Args>
int probe(Args&&... args)
{
  pair<const Args&...> refs(args...);
  return refs.first + refs.second;
}

int main()
{
  return probe(1, 4) == 5 ? 0 : 1;
}
