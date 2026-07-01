function @clobber(%a : i64, %b : i64, %c : i64, %d : i64, %e : i64, %f : i64) -> i64 {
  block ^entry:
    return i64 %e
}

function @size(%this : ptr) -> i64 {
  block ^entry:
    %ignored = call i64 @clobber(10, 20, 30, 40, 12345, 60)
    return i64 1
}

function @check(%this : ptr, %index : i64) -> i64 {
  slot $this : ptr
  slot $index : i64

  block ^entry:
    store ptr %this, $this
    store i64 %index, $index
    %saved = load i64 $index
    %obj = load ptr $this
    %n = call i64 @size(%obj)
    %bad = cmp uge i64 %saved, %n
    branch %bad, ^bad, ^good

  block ^bad:
    return i64 3

  block ^good:
    return i64 0
}

function @main() -> i64 [role=entry] {
  block ^entry:
    %result = call i64 @check(0, 0)
    return i64 %result
}
