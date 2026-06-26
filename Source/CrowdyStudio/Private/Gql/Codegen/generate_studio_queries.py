#!/usr/bin/env python3
"""Generate CrowdyStudio GraphQL query-string constants from the Crowded Kingdoms SDL.

This is the Phase 2 "stand up codegen" step. It reads the schema that ships in the
cks-docs repo (static/schema/*.graphql), and for a fixed manifest of operations it:

  * validates the root field exists and reports any drift, and
  * emits a query string whose selection set is expanded from the return type's
    scalar/enum fields (recursing into nested objects up to a small depth).

Arguments map straight through as GraphQL variables, so an input-object argument
(e.g. `input: CreateAccessTierInput!`) becomes `$input: CreateAccessTierInput!` and
the caller supplies the object — exactly how the hand-written editor ops build it.

The hand-written bodies in CrowdyStudioQueries.cpp stay the source the editor compiles
against; this generated header is the schema-grounded reference to diff against when the
schema moves. Regenerate after a schema bump and reconcile any differences.

Usage:
    python generate_studio_queries.py [SCHEMA_DIR] [OUTPUT_HEADER]

Defaults resolve the sibling cks-docs checkout and Gql/Generated/ next to this script.
"""

import os
import re
import sys

# Operations the studio console uses, by root field name. The generator looks each up in
# the merged Query/Mutation roots across all schema files (management + game).
QUERY_OPS = [
    "myOrganizations", "myApps", "app", "orgEnvironments",
    "runtimePermissions", "appAccessTiers",
    "teams", "teamPolicy", "channels", "channelPolicy",
    "teamMembers", "teamRoles", "channelMembers", "channelRoles",
    "nearbyGridPermissions", "gridPermissionLimits", "gridGroupGrants", "gridUserPermissions",
    "gameModelContainerTypes", "gameModelPropertyDefs", "gameModelFunctions",
    "gameModelFeatures", "gameModelTierFeatures", "gameModelPolicy",
    "gameModelContainers", "gameModelContainerState",
]
MUTATION_OPS = [
    "login", "createOrganization", "createApp", "updateApp", "archiveApp",
    "linkAppToEnvironment",
    "setTeamPolicy", "setChannelPolicy", "createChannel", "createTeam",
    "deleteTeam", "deleteChannel", "updateTeam", "updateChannel",
    "addTeamMember", "removeTeamMember", "setTeamMemberRoles",
    "createTeamRole", "updateTeamRole", "deleteTeamRole",
    "addChannelMember", "removeChannelMember", "setChannelMemberRoles",
    "createChannelRole", "updateChannelRole", "deleteChannelRole",
    "createGrid", "grantGridPermissions", "revokeGridPermissions", "setGridPermissionLimits",
    "assignGroupToGrid", "revokeGroupFromGrid",
    "gameModelUpsertContainerType", "gameModelUpsertPropertyDef", "gameModelUpsertFunction",
    "gameModelDeleteFunction", "gameModelDefineFeature", "gameModelGrantTierFeature",
    "gameModelRevokeTierFeature", "gameModelSetPolicy", "gameModelSeed",
]

BUILTIN_SCALARS = {"Int", "Float", "String", "Boolean", "ID"}
MAX_SELECTION_DEPTH = 3


def strip_comments(text):
    text = re.sub(r'"""(?:.|\n)*?"""', " ", text)
    text = re.sub(r'#.*', " ", text)
    return text


def extract_blocks(text, keyword):
    """Yield (name, body) for every `keyword Name { ... }` block, brace-matched."""
    for match in re.finditer(rf'\b{keyword}\s+(\w+)[^{{}}]*\{{', text):
        name = match.group(1)
        depth = 1
        index = match.end()
        start = index
        while index < len(text) and depth > 0:
            if text[index] == '{':
                depth += 1
            elif text[index] == '}':
                depth -= 1
            index += 1
        yield name, text[start:index - 1]


def base_type(type_str):
    return type_str.replace("!", "").replace("[", "").replace("]", "").strip()


def parse_field_line(line):
    """Parse `name(args): ReturnType` -> (name, args_text, return_type) or None."""
    m = re.match(r'\s*(\w+)\s*(\([^)]*\))?\s*:\s*([\[\]\w!]+)', line)
    if not m:
        return None
    return m.group(1), (m.group(2) or "")[1:-1], m.group(3)


def parse_args(args_text):
    """Parse `a: T!, b: U` -> [(a, T!), (b, U)] tolerating nested commas in defaults."""
    args = []
    for piece in re.findall(r'(\w+)\s*:\s*([\[\]\w!]+)', args_text):
        args.append((piece[0], piece[1]))
    return args


def build_schema(schema_dir):
    text = ""
    for name in sorted(os.listdir(schema_dir)):
        if name.endswith(".graphql"):
            with open(os.path.join(schema_dir, name), "r", encoding="utf-8") as handle:
                text += "\n" + handle.read()
    text = strip_comments(text)

    scalars = set(BUILTIN_SCALARS)
    scalars.update(re.findall(r'\bscalar\s+(\w+)', text))
    for enum_name, _ in extract_blocks(text, "enum"):
        scalars.add(enum_name)

    # Query/Mutation are named blocks like any other type ("Query" is the name); they may also
    # arrive via `extend type Query { ... }`, so merge fields from every block we see for them.
    objects = {}
    roots = {"Query": {}, "Mutation": {}}
    for kind in ("type", "input"):
        for name, body in extract_blocks(text, kind):
            if name in roots:
                for line in body.splitlines():
                    parsed = parse_field_line(line)
                    if parsed:
                        roots[name][parsed[0]] = (parsed[1], parsed[2])
                continue
            # Shared types are defined in more than one schema file (and may be `extend`ed),
            # so merge by field name to avoid selecting the same field twice.
            fields = objects.setdefault(name, [])
            seen = {field_name for field_name, _ in fields}
            for line in body.splitlines():
                parsed = parse_field_line(line)
                if parsed and parsed[0] not in seen:
                    fields.append((parsed[0], parsed[2]))
                    seen.add(parsed[0])
    return scalars, objects, roots


def build_selection(type_name, scalars, objects, depth, visited):
    if type_name in scalars or type_name not in objects or depth > MAX_SELECTION_DEPTH:
        return ""
    if type_name in visited:
        return ""
    parts = []
    for field_name, field_type in objects[type_name]:
        inner = base_type(field_type)
        if inner in scalars or inner not in objects:
            parts.append(field_name)
        else:
            nested = build_selection(inner, scalars, objects, depth + 1, visited | {type_name})
            if nested:
                parts.append(f"{field_name} {{ {nested} }}")
    return " ".join(parts)


def build_query(op_name, kind, roots, scalars, objects):
    field = roots[kind].get(op_name)
    if field is None:
        return None
    args_text, return_type = field
    args = parse_args(args_text)

    var_decls = ", ".join(f"${name}: {gql_type}" for name, gql_type in args)
    var_decls = f"({var_decls})" if args else ""
    arg_pass = ", ".join(f"{name}: ${name}" for name, _ in args)
    arg_pass = f"({arg_pass})" if args else ""

    selection = build_selection(base_type(return_type), scalars, objects, 1, set())
    selection = f" {{ {selection} }}" if selection else ""

    operation = "query" if kind == "Query" else "mutation"
    pascal = op_name[0].upper() + op_name[1:]
    return f"{operation} Studio{pascal}{var_decls} {{ {op_name}{arg_pass}{selection} }}"


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    default_schema = os.path.normpath(os.path.join(
        script_dir, *([".."] * 8), "cks-docs", "static", "schema"))
    default_output = os.path.normpath(os.path.join(script_dir, "..", "Generated",
                                                    "CrowdyStudioGeneratedQueries.h"))

    schema_dir = sys.argv[1] if len(sys.argv) > 1 else default_schema
    output_path = sys.argv[2] if len(sys.argv) > 2 else default_output

    if not os.path.isdir(schema_dir):
        print(f"[codegen] schema dir not found: {schema_dir}", file=sys.stderr)
        return 1

    scalars, objects, roots = build_schema(schema_dir)

    lines = [
        "// GENERATED by Gql/Codegen/generate_studio_queries.py from cks-docs/static/schema.",
        "// Do not edit by hand. Regenerate after a schema change and reconcile with the",
        "// hand-written bodies in CrowdyStudioQueries.cpp.",
        "#pragma once",
        "#include \"CoreMinimal.h\"",
        "",
        "namespace CrowdyStudioGeneratedGql",
        "{",
    ]

    missing = []
    for kind, op_list in (("Query", QUERY_OPS), ("Mutation", MUTATION_OPS)):
        for op_name in op_list:
            query = build_query(op_name, kind, roots, scalars, objects)
            if query is None:
                missing.append(op_name)
                continue
            pascal = op_name[0].upper() + op_name[1:]
            escaped = query.replace("\"", "\\\"")
            lines.append(f"\tstatic const TCHAR* {pascal} = TEXT(\"{escaped}\");")

    lines.append("}")
    lines.append("")

    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines))

    print(f"[codegen] wrote {output_path}")
    if missing:
        print(f"[codegen] WARNING: ops not found in schema (drift?): {', '.join(missing)}",
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
