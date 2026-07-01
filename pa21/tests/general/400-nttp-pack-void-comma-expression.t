template <class A, class B>
struct Same {
  static const bool value = false;
};

template <class A>
struct Same<A, A> {
  static const bool value = true;
};

template <bool...>
struct AllDummy;

template <bool... Pred>
struct All : Same<AllDummy<Pred...>, AllDummy<((void)Pred, true)...> > {};

int main() {
  return All<true, true, true>::value ? 0 : 1;
}
