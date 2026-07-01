struct iterator
{
  void *ptr;
};

struct bucket
{
  iterator first;
  iterator last;
};

unsigned count()
{
  return 3;
}

int main()
{
  bucket *buckets = new bucket[count()];
  buckets[1].first.ptr = buckets;
  int ok = buckets[1].first.ptr == buckets;
  delete[] buckets;
  return ok ? 0 : 1;
}
// VALIDATION: compile-pass
