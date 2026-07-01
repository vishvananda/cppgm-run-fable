global @YB__vtable = {
  ptr addr @YB__f
  ptr addr @YB___YB
}
global @YD__vtable = {
  ptr addr @YD__f
  ptr addr @YD___YD
}

function @YB__f(%this : ptr) -> i64 {
  slot $this : ptr

  block ^entry:
    store ptr %this, $this
    return i64 1
}
function @YB___YB(%this : ptr) -> void {
  slot $this : ptr

  block ^entry:
    store ptr %this, $this
    %t1 = load ptr $this
    %t2 = addr @YB__vtable
    store ptr %t2, %t1
    return void
}
function @YB__YB(%this : ptr) -> void {
  slot $this : ptr

  block ^entry:
    store ptr %this, $this
    %t1 = load ptr $this
    %t2 = addr @YB__vtable
    store ptr %t2, %t1
    return void
}
function @YD__f(%this : ptr) -> i64 {
  slot $this : ptr

  block ^entry:
    store ptr %this, $this
    return i64 2
}
function @YD__YD(%this : ptr) -> void {
  slot $this : ptr

  block ^entry:
    store ptr %this, $this
    %t1 = load ptr $this
    call void @YB__YB(%t1)
    %t2 = load ptr $this
    %t3 = addr @YD__vtable
    store ptr %t3, %t2
    return void
}
function @YD___YD(%this : ptr) -> void {
  slot $this : ptr

  block ^entry:
    store ptr %this, $this
    %t1 = load ptr $this
    %t2 = addr @YD__vtable
    store ptr %t2, %t1
    %t3 = load ptr $this
    call void @YB___YB(%t3)
    return void
}
function @g(%b : ptr) -> i64 {
  slot $b : ptr

  block ^entry:
    store ptr %b, $b
    %t1 = load ptr $b
    %t2 = load ptr %t1
    %t3 = load ptr %t2
    %t4 = call i64 %t3(%t1) as (%arg0 : ptr) -> i64
    return i64 %t4
}
function @main() -> i64 [role=entry] {
  slot $d__0 : i64

  block ^entry:
    %t1 = addr $d__0
    call void @YD__YD(%t1)
    %t2 = addr $d__0
    %t3 = call i64 @g(%t2)
    %t4 = addr $d__0
    call void @YD___YD(%t4)
    return i64 %t3
}
