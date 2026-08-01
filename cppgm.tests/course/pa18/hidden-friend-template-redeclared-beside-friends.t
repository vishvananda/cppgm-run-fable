struct A
{
	int v;

	friend int take(A x) { return x.v; }

	template <typename T>
	friend int take(T* p);
};

template <typename T>
int take(T* p) { return *p + 1; }

int main()
{
	A a;
	a.v = 5;
	int x = 3;
	return take(a) == 5 && take(&x) == 4 ? 0 : 1;
}
