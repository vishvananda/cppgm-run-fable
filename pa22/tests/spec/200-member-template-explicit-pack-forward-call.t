// VALIDATION: compile-pass
// N3485 focus: 14.5.2 [temp.mem], 14.5.3 [temp.variadic], 14.8.1 [temp.arg.explicit]

template<class T>
T && forward(T & t)
{
  return static_cast<T &&>(t);
}

template<class KeyT, class... Args>
int count(Args&&...)
{
  return sizeof...(Args);
}

struct box
{
  template<class... Args>
  int g(Args&&... args)
  {
    return count<int>(forward<Args>(args)...);
  }
};

int main()
{
  box b;
  return b.g(7) == 1 ? 0 : 1;
}
