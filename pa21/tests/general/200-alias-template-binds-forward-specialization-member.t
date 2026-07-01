template<class T>
struct Payload;

template<class T>
struct Holder
{
  T value;
};

struct Item
{
  int value;
};

template<class T>
struct Payload
{
  long count;
};

template<class T>
using Buffer = Holder<Payload<T> >;

Buffer<Item> global_holder;

int main()
{
  global_holder.value.count = 7;
  return global_holder.value.count == 7 ? 0 : 1;
}
