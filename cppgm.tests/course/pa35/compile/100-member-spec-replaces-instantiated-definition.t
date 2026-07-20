// PA35 14.7.3p18: an explicit member-template definition replaces the
// definition the enclosing specialization instantiated from the
// primary's pattern.
template<class T> struct Owner
{
	template<class U> int member(U) { return 1; }
};
Owner<int> instantiated;
template<> template<class U> int Owner<int>::member(U) { return 2; }
int main()
{
	Owner<int> p;
	return p.member(3) == 2 ? 0 : 1;
}
