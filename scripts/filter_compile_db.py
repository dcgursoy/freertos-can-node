#!/usr/bin/env python3
"""Write a compile database that only contains the project's own translation units.

    filter_compile_db.py [--plain-includes] <compile_commands.json> <output dir> <repo root>
                         <dir> [<dir> ...]

The static analysers are pointed at the output directory. Third-party sources (the FreeRTOS
kernel, Unity) are compiled by the same build but must not be analysed, and the simplest
reliable way to keep them out is to not list them.

--plain-includes rewrites -isystem to -I. The build marks third-party include directories as
system directories so their headers raise no compiler warnings, but cppcheck does not search
system directories at all: without this option it would not see the FreeRTOS headers, and
every kernel macro and function would be unknown to it.

Uses the standard library only.
"""
import json
import os
import sys


def main() -> int:
    args = sys.argv[1:]
    plain_includes = False
    if args and args[0] == "--plain-includes":
        plain_includes = True
        args = args[1:]

    if len(args) < 4:
        print(__doc__, file=sys.stderr)
        return 2

    source_db, out_dir, root = args[0], args[1], os.path.realpath(args[2])
    keep_dirs = [os.path.join(root, d) + os.sep for d in args[3:]]

    with open(source_db, encoding="utf-8") as handle:
        entries = json.load(handle)

    kept = []
    for entry in entries:
        path = entry["file"]
        if not os.path.isabs(path):
            path = os.path.join(entry["directory"], path)
        path = os.path.realpath(path)
        if any(path.startswith(d) for d in keep_dirs):
            if plain_includes:
                if "command" in entry:
                    entry["command"] = entry["command"].replace(" -isystem ", " -I")
                if "arguments" in entry:
                    rewritten = []
                    make_plain = False
                    for argument in entry["arguments"]:
                        if argument == "-isystem":
                            make_plain = True
                            continue
                        rewritten.append("-I" + argument if make_plain else argument)
                        make_plain = False
                    entry["arguments"] = rewritten
            kept.append(entry)

    if not kept:
        print("filter_compile_db: no project translation unit matched", file=sys.stderr)
        return 1

    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "compile_commands.json"), "w", encoding="utf-8") as handle:
        json.dump(kept, handle, indent=1)

    print(f"filter_compile_db: kept {len(kept)} of {len(entries)} translation units")
    return 0


if __name__ == "__main__":
    sys.exit(main())
