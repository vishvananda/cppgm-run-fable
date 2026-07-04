// N3485 focus: 14.7.1 [temp.inst] p1: naming a specialization while
// only the forward declaration is visible never instantiates it - the
// definition appearing later must leave the dormant record alone (the
// bad member never analyzes because nothing demands completeness),
// and the first completeness demand instantiates through the ordinary
// body path, partial-specialization match included.
template<class T> struct Holder;
typedef Holder<int>* dormant_use;
template<class T> struct Holder { typename T::missing m; };

template<class T> struct Pick;
typedef Pick<char*> named_early;
template<class T> struct Pick { static const int value = 1; };
template<class T> struct Pick<T*> { static const int value = 2; };

int main() { return named_early::value - 2; }
