template<class T>
T declval();

int sink(int *&&);
int sink_const(int * const &);

template<class... Args>
auto forward_pointer(Args&&... args)
  -> decltype(sink(static_cast<Args&&>(args)...));

template<class... Args>
auto forward_const_pointer(Args&&... args)
  -> decltype(sink_const(static_cast<Args&&>(args)...));

int main() {
  return sizeof(forward_pointer(declval<int *>())) == sizeof(int) &&
         sizeof(forward_const_pointer(declval<int * const &>())) == sizeof(int) ?
      0 : 1;
}
