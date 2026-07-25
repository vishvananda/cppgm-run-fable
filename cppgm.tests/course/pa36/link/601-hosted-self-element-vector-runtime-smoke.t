# hosted self-referential-element vector smoke: a class whose first
# member is std::vector of the class itself, exercised through
# push_back and insert. Completing the member's vector<A> while A is
# still open replays the specialization with eager conditional
# noexcept evaluation, which instantiates completeness-checked traits
# over the open class and poisons their memoized records; the
# poisoned bodies must heal at the end-of-unit retry (deferred
# noexcept specs, retryable partial-specialization records, restored
# pending member-class definitions, and a re-scanned const-eval body
# index). Reduced from dev/src/sema/type_builder.cpp and five sibling
# TUs of the PA39 self-host build.
