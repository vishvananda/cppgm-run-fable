global @box : i64 = 7

function @id(%x : ptr) -> ptr {
  block ^entry:
    return ptr %x
}

function @wrap(%this : ptr, %pf : ptr) -> ptr {
  slot $this : ptr
  slot $pf : ptr

  block ^entry:
    store ptr %this, $this
    store ptr %pf, $pf
    %t1 = load ptr $this
    %t2 = load ptr $pf
    %t3 = call ptr %t2(%t1) as (%arg0 : ptr [pass=reference]) -> ptr
    return ptr %t3
}

function @main() -> i64 [role=entry] {
  block ^entry:
    %p = addr @box
    %f = addr @id
    %r = call ptr @wrap(%p, %f)
    %ok = cmp eq ptr %r, %p
    return i64 %ok
}
