function @bump(%cell : ptr) -> void {
  slot $cell : ptr

  block ^entry:
    store ptr %cell, $cell
    %t1 = load ptr $cell
    %t2 = load i32 %t1
    %t3 = binary add i32 %t2, 1
    store i32 %t3, %t1
    return void
}

function @main() -> i32 [role=entry, binding=strong] {
  slot $counter : obj<8x4>

  block ^entry:
    %base = addr $counter
    %cell = index i32 [projection=field] %base, 0
    store i32 0, %cell
    jump ^loop_cond

  block ^loop_cond:
    %v = load i32 %cell
    %done = cmp ge i32 %v, 5
    branch %done, ^loop_end, ^loop_body

  block ^loop_body:
    call void @bump(%cell)
    jump ^loop_cond

  block ^loop_end:
    %base2 = addr $counter
    %cell2 = index i32 [projection=field] %base2, 0
    %r = load i32 %cell2
    %ok = cmp eq i32 %r, 5
    branch %ok, ^good, ^bad

  block ^good:
    return i32 0

  block ^bad:
    return i32 1
}
