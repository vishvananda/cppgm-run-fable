template<class T>
T&& declval();

struct Id {
  template<class U>
  U&& operator()(U&&) const;
};

template<class _Fp, class... _Args>
struct invoke_result {
  typedef decltype(static_cast<_Fp&&>(declval<_Fp>())(
      static_cast<_Args&&>(declval<_Args>())...)) type;
};

invoke_result<Id&, int&>::type g();

int main() {
  return 0;
}
