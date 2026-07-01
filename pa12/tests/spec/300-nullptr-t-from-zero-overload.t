// N3485 focus: 4.10 [conv.ptr] null pointer constant conversion to nullptr_t
int selected;

void take(nullptr_t)
{
  selected = 1;
}

int run()
{
  take(0);
  return selected == 1 ? 0 : 1;
}
