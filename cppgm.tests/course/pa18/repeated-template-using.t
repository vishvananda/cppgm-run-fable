namespace n {
struct S {};
template <typename T> struct V { T item; };
}

using n::S;
using n::S;
using n::V;
using n::V;

int touch(int x);

int main() {
  S s;
  V<int> v;
  v.item = 3;
  return touch(v.item);
}
