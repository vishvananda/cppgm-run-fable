set -eu
nm "__OBJ1__" | c++filt | grep -Eq 'Box<int>::Box<int, void>\(int\)'
echo host_ooc_constructor_template_default_arg_definition 1
