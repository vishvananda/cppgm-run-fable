namespace n {

template <typename T>
void swap(T& a, T& b)
{
	T c(a);
	a = b;
	b = c;
}

struct BR
{
	bool* p;

	friend void swap(BR x, BR y)
	{
		bool t = *x.p;
		*x.p = *y.p;
		*y.p = t;
	}

	friend void swap(BR x, bool& y)
	{
		bool t = *x.p;
		*x.p = y;
		y = t;
	}
};

}

int main()
{
	int a = 1, b = 2;
	n::swap(a, b);
	bool v = false, w = true;
	n::BR r;
	r.p = &v;
	swap(r, w);
	return a == 2 && b == 1 && v && !w ? 0 : 1;
}
