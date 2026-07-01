set -eu
nm "__OBJ1__" | c++filt | grep -Eq 'Box<int>::Box<int>\(int\)'
echo host_ooc_constructor_template_definition 1
