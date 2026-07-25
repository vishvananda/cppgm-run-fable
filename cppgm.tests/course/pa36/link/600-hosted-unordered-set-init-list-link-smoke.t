# hosted unordered_set initializer-list and range construction smoke:
# the _Hashtable tagged constructor template is selected (through the
# delegating range constructor) before its out-of-class definition is
# replayed for the instantiation, which must still bind the body.
