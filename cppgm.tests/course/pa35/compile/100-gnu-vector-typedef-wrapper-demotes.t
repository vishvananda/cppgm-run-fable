// PA35: a GNU vector typedef (vector_size) carries the vector fact;
// a gnu_inline wrapper returning a vector literal demotes instead of
// miscompiling the literal as a scalar cast.
typedef float __v4f_probe __attribute__ ((__vector_size__ (16), __may_alias__));
extern __inline __v4f_probe __attribute__((__gnu_inline__, __always_inline__))
wrapper_setzero(void)
{
	return __extension__ (__v4f_probe){ 0.0f, 0.0f, 0.0f, 0.0f };
}
static_assert(sizeof(float) == 4, "anchor");
int main() { return 0; }
