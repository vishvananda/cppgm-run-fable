# hosted static-member aggregate array smoke: a static data member
# array of aggregates with a runtime-initialized element clones its
# constructor actions into the dynamic init path. The actions must
# carry their subscripted element address explicitly - the member
# binding's home is its class scope, and the shared-base offset form
# only serves contexts that supply the base object - or the cloned
# call lowers with no object argument. Reduced from the PA39
# aggregate-array audit.
