// N3485 focus: 13.3.3.2 [over.ics.rank] nullptr_t conversion ties ordinary conversion
int take(long)
{
  return 1;
}

int take(nullptr_t)
{
  return 2;
}

int run()
{
  return take(0);
}
