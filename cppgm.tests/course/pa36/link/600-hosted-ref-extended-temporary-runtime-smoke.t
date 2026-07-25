# hosted reference-extended temporary smoke: a call temporary bound to
# a reference (directly and through range-for's hidden __range) lives
# for the reference's lifetime; reading it after later calls must see
# intact contents.
