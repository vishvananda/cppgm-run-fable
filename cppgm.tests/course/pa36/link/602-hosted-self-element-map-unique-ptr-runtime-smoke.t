# hosted map-of-unique_ptr smoke: a class template member body looks
# an element up in std::map<std::string, std::unique_ptr<T>> and
# dereferences through unique_ptr (std::get<0> over the tuple-backed
# pointer storage), instantiated for a class that holds the holder as
# its first member. The specialization bodies flush while the element
# class is still open; the poisoned bodies and the trait partials
# they instantiated (through __is_move_insertable's allocator partial
# specialization) must all heal at the end-of-unit retry. Reduced
# from the map<string, unique_ptr<ClassSpecialization>> members the
# PA39 self-host build compiles in dev/src/sema.
