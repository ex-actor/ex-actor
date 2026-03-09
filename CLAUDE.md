refer README.md and docs/contents for how to use this framework(ignore files listed in .gitignore, which might be stale)

refer "How to build from source" part in CONTRIBUTING.md for how to build&test.

when debugging tests, do not simplify the test code unless you are very confident the test is not correct.

# Code Style

Based on Google Style Guide, with the following additions:

- For all args that can't get its meaning from the variable name, add a `/*arg_name=*/` comment before the arg.