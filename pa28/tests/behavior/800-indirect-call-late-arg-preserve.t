global @box : i8 = 7
global @strlit [binding=internal] = {
  i8 0
}
global @dispatch [storage=readonly] = {
  ptr addr @sink
}
global @g_num : i64 = 0
global @g_type : i32 = 0
global @g_nbytes : i64 = 0

function @data(%this : ptr) -> ptr {
  block ^entry:
    return ptr %this
}

function @size(%this : ptr) -> i64 {
  block ^entry:
    return i64 1
}

function @sink(%this : ptr, %src : ptr, %num : i64, %type : i32, %data : ptr, %nbytes : i64) -> void {
  block ^entry:
    store i64 %num, @g_num
    store i32 %type, @g_type
    store i64 %nbytes, @g_nbytes
    return void
}

function @main() -> i64 [role=entry] {
  block ^entry:
    %obj = addr @box
    %src = addr @strlit
    %num = copy i64 1
    %p = call ptr @data(%obj)
    %n = call i64 @size(%obj)
    %tbl = addr @dispatch
    %fp = load ptr %tbl
    call void %fp(%obj, %src, %num, 13, %p, %n) as (%arg0 : ptr, %arg1 : ptr, %arg2 : i64, %arg3 : i32, %arg4 : ptr, %arg5 : i64) -> void
    %a = load i64 @g_num
    %b = cmp eq i64 %a, 1
    branch %b, ^rhs, ^short
  block ^rhs:
    %c = load i32 @g_type
    %d = cmp eq i32 %c, 13
    %e = cmp ne i64 %d, 0
    branch %e, ^rhs2, ^short
  block ^rhs2:
    %f = load i64 @g_nbytes
    %g = cmp eq i64 %f, 1
    %h = cmp ne i64 %g, 0
    return i64 %h
  block ^short:
    return i64 0
}
