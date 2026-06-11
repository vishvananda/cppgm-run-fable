# Shared Source Grammar

`source.gram` is the canonical source-language grammar for `cppgm++` frontend
assignments starting at PA10.

The local `paN/paN.gram` files for PA10-PA12, PA14-PA27, and PA29 expose
this shared grammar under each assignment's filename. README files
should use this standard wording:

> The authoritative source syntax is the shared `cppgm++` source grammar,
> exposed for this assignment as `paN.gram`. The grammar defines accepted syntax
> only; the PA-specific semantic and lowering requirements are defined by the
> assignment boundary and out-of-scope sections.

A construct being accepted by this grammar does not make its semantic analysis
or code generation required in every assignment.
