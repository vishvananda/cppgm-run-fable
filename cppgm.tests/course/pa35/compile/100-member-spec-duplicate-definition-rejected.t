// PA35 audit: two explicit definitions of one member template of a
// specialization are an ODR redefinition (14.7.3), not a silent
// replacement.
template<class T> struct Owner { template<class U> void member(U); };
template<> template<class U> void Owner<int>::member(U) {}
template<> template<class U> void Owner<int>::member(U) {}
int main() { return 0; }
