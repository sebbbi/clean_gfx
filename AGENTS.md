# Coding Guidelines

- Write simple, efficient C/C++ code.
- Avoid lambdas and complex templates.
- Avoid trivial single-line wrapper functions.
- Avoid memory allocations, including short-lived local vectors.
- Do not use `std::shared_ptr`.
- Avoid `std::unique_ptr`, especially when the value can be stored directly as a data member.
- Do not use class inheritance or virtual functions.
- Do not use PIMPL interfaces.
- Avoid standard-library algorithms; prefer straightforward loops.
- Do not use hash maps or ordered maps.
- Do not use mutexes or atomics. The API is intentionally single-threaded and is not thread-safe yet.
- Avoid copying user data structures. Prefer references to structures, and use spans for array data in structures and function parameters.
- Use a custom span type represented by a pointer and size. It must support construction from an initializer list so variable-length arguments remain concise. An initializer list passed as a function argument remains alive through that function call; do not retain a span backed by it after the call returns.
- Use C++20 designated initializers with named fields for structures.
- Give public API structure fields useful default values. At call sites, initialize only the non-default fields and name every initialized field.
- Always review code for performance issues before considering work complete.
