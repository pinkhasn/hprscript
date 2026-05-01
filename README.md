# hprscript
`hprscript` is a command-line multi-pattern search tool. It scans files **once** and matches **all patterns simultaneously** using Intel's Hyperscan regex engine, replacing N sequential `grep`/`rg` calls with a single invocation. Patterns use **PCRE** syntax (the subset Hyperscan accepts).
