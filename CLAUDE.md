refer README.md and docs/contents for how to use this framework(ignore files listed in .gitignore, which might be stale)

refer "How to build from source" part in CONTRIBUTING.md for how to build&test.

when debugging tests, do not simplify the test code unless you are very confident the test is not correct.

when refactoring, do not remove original comments unless they are stale.

# Code Style

Based on Google Style Guide, with the following additions:

- For all args that can't get its meaning from the variable name, add a `/*arg_name=*/` comment before the arg.
- No one-line if/while/for/switch statements. Always use the `{}` block.
- Don't std::pair, first/second is hard to read, always define a struct for better readability.
- When using std::pair or iterators, don't use first/second or iterator->first/second, instead use struct binding like `auto& [key, value] = pair`, or alias like `auto& meaningful_name = iterator->second` if only one field is needed.
- Don't use placeholder `_` in struct binding. Always use meaningful variable names even it's not used.
- In maps, when you can't get the key/value meaning from the variable name, add comments like `map</*key_meaning*/KeyType, /*value_meaning*/ValueType/>` for better readability. No "=" sign.