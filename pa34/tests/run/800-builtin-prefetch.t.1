#if !__has_builtin(__builtin_prefetch)
#error expected __builtin_prefetch
#endif

int observe(int *p)
{
  *p = *p + 1;
  return *p;
}

int main()
{
  int value = 0;
  __builtin_prefetch(&value);
  __builtin_prefetch(&value, 1);
  __builtin_prefetch(&value, 0, 3);
  __builtin_prefetch(&value + observe(&value), 0, 3);
  return value == 1 ? 0 : 1;
}
