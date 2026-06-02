# Vendored Third-Party Libraries

This directory contains third-party libraries vendored directly into ex_actor to avoid external dependencies.

## Structure

```
3rd_lib/
├── absl/                   # Subset of abseil-cpp (LTS 20260107.1)
│   ├── base/               # Platform detection, macros, config
│   ├── functional/         # AnyInvocable (move-only type-erased callable with SBO)
│   │   └── internal/
│   ├── meta/               # Type traits utilities
│   └── utility/            # Utility helpers
├── daking/                 # Lock-free MPSC queue
├── moody_camel_queue/      # High-performance concurrent queue
└── README.md
```

## Namespace and Macro Convention

All vendored libraries are placed under the `ex_actor::embedded_3rd` namespace to avoid conflicts with users who may have their own versions of the same libraries.

| Library | Original Namespace | Vendored Namespace |
|---------|-------------------|-------------------|
| abseil  | `absl`            | `ex_actor::embedded_3rd::absl` |
| daking  | `daking`          | `ex_actor::embedded_3rd::daking` |
| moodycamel | `moodycamel`  | `ex_actor::embedded_3rd::moodycamel` |

For libraries that define macros (e.g., abseil's `ABSL_*`), prefix all macros with `EX_ACTOR_` to avoid collisions. For example: `ABSL_PREDICT_TRUE` becomes `EX_ACTOR_ABSL_PREDICT_TRUE`.

## How to Add a New Vendored Library

1. **Create a subdirectory** under `3rd_lib/` named after the library.

2. **Copy only the headers you need** — trace the include graph from the entry-point header and copy the minimal set of files that satisfies all `#include` directives.

3. **Rename the namespace** to `ex_actor::embedded_3rd::<library_name>`:
   ```bash
   find 3rd_lib/<lib>/ -name "*.h" -exec sed -i 's/\bnamespace <orig>\b/namespace ex_actor::embedded_3rd::<orig>/g' {} +
   find 3rd_lib/<lib>/ -name "*.h" -exec sed -i 's/\b<orig>::/ex_actor::embedded_3rd::<orig>::/g' {} +
   ```

4. **Rename macros** with a unique prefix (e.g., `EX_ACTOR_<LIB>_`):
   ```bash
   find 3rd_lib/<lib>/ -name "*.h" -exec sed -i 's/\b<ORIG_PREFIX>_/EX_ACTOR_<ORIG_PREFIX>_/g' {} +
   ```

5. **Fix include paths** to point to the new location:
   ```bash
   find 3rd_lib/<lib>/ -name "*.h" -exec sed -i 's|#include "<orig>/|#include "ex_actor/3rd_lib/<lib>/|g' {} +
   ```

6. **Disable inline/versioned namespaces** if present (e.g., abseil's `ABSL_OPTION_USE_INLINE_NAMESPACE`) — they are redundant once the outer namespace is renamed.

7. **Verify no collateral damage** — if other files in `3rd_lib/` were accidentally modified by your sed commands, restore them with `git checkout`.

8. **Test** — write a minimal .cc that includes the header and exercises the vendored API, then rebuild the full project and run tests.

## How to Upgrade an Existing Vendored Library

1. Delete the library's subdirectory.
2. Re-download from the desired release tag.
3. Re-apply steps 2-8 from "How to Add" above.
4. Update the version noted in this README's structure section.
