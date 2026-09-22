# Understanding This Project

- Refer README.md and docs/contents for how to use this framework(ignore files listed in .gitignore, which might be stale)
- Refer "How to build from source" part in CONTRIBUTING.md for how to build&test.

# Editing Guidelines

- When editing code, try best to keep original comments unless they are stale.

# Debugging

- When debugging tests, do not simplify the test code unless you are very confident the test is not correct.
- Prefer adding logs to gather more information before thinking too much.

# Code Style

Based on Google Style Guide, with the following additions:

- For all args that can't get its meaning from the variable name, add a `/*arg_name=*/` comment before the arg.
- No one-line if/while/for/switch statements. Always use the `{}` block.
- Don't std::pair, first/second is hard to read, always define a struct for better readability.
- When using std::pair or iterators, don't use first/second or iterator->first/second, instead use struct binding like `auto& [key, value] = pair`, or alias like `auto& meaningful_name = iterator->second` if only one field is needed.
- Don't use placeholder `_` in struct binding. Always use meaningful variable names even it's not used.
- In maps, when you can't get the key/value meaning from the variable name, add comments like `map</*key_meaning*/KeyType, /*value_meaning*/ValueType/>` for better readability. No "=" sign.

# Pull Request Description & Commit Message

- Make the description short and concise so reviewer can quickly understand the changes. don't exceed 50 words. Make the structure compact, no big titles, so it can be directly used as squash merge's commit message.

