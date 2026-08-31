# HNN backend adapter

Signal's public HNN API and 1,024-dimensional memory contract do not change.
`shared/holographic_nn_backend.*` is a private numeric adapter behind that API.

## Build selection

The built-in radix-2 backend remains the default:

```sh
cmake -S . -B build -DSIGNAL_HNN_BACKEND=builtin
```

The pinned liblecore dependency can be selected in correctness or optimized
mode:

```sh
cmake -S . -B build-direct -DSIGNAL_HNN_BACKEND=lecore-direct
cmake -S . -B build-radix2 -DSIGNAL_HNN_BACKEND=lecore-radix2
```

Selecting either liblecore backend also enables the pinned dependency. The old
`SIGNAL_HNN_ENABLE_LECORE=ON` option still compiles liblecore without selecting
it, which is useful for conformance tests.

## Preserved behavior

- Signal still owns key generation, action IDs, feature encoding, memory
  capacity, and deterministic tie-breaking.
- Bind and unbind results are normalized before they leave the adapter.
- Bundles keep input order and return a unit vector.
- Similarity and compatible action cleanup use liblecore cosine operations.
- Zero vectors remain zero. Non-finite input fails closed to a zero vector or a
  zero score. The thread-local diagnostic status records the reason.

## Context and memory ownership

Each thread lazily creates one liblecore context for each liblecore backend it
uses. Context memory comes from a fixed 32 KiB thread-local arena. The adapter
also owns fixed thread-local bundle work buffers. There is no heap allocation,
shared mutable context, or allocation after a context is initialized.

The conformance tests initialize both liblecore modes in one process and check
normalization, bind/unbind, bundle order, cosine cleanup ties, all nine actions,
full and overloaded memories, thread isolation, and stable allocation counts.
The built-in backend stays available as the numerical reference.
