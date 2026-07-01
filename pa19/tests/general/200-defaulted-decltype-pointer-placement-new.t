typedef decltype(sizeof(0)) size_t;

void * operator new(size_t, void * ptr);

template<class T>
T && declval();

struct Item {
  int value;
};

template<class T, class... Args,
         class = decltype(::new (declval<void *>()) T(declval<Args>()...))>
T * construct_at(T * ptr, Args &&... args)
{
  return ::new ((void *)ptr) T(static_cast<Args &&>(args)...);
}

int main()
{
  Item * src = 0;
  Item * dst = 0;
  construct_at(&dst, src);
  return dst == src ? 0 : 1;
}
