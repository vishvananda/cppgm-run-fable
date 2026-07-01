template<class T>
struct traits {};

template<class CharT, class Traits = traits<CharT> >
class V {
public:
  typedef unsigned long size_type;

private:
  struct Tag {};
  V(Tag, const CharT*, size_type);

  template<class, class, class>
  friend class S;
};

template<class CharT, class Traits, class Alloc>
class S {
  typedef V<CharT, Traits> SelfView;

  const CharT* data() const;
  unsigned long size() const;

public:
  operator SelfView() const {
    return SelfView(typename SelfView::Tag(), data(), size());
  }
};

typedef S<char, traits<char>, int> Str;
typedef V<char, traits<char> > View;

View make(Str s) {
  return s;
}
