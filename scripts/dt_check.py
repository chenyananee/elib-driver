#!/usr/bin/env python3
"""dt_check.py - parse C device-tree node arrays into a review tree.

The script understands the array style used by elib-driver:

    static const elib_drv_dt_node_t children[] = {
        { .name = "sensor", .fops = &sensor_fops, .ctx = &sensor_cfg,
          .controller = "bus", .flags = 0u, .lock_timeout_ms = 100u,
          .children = NULL, .child_count = 0u },
    };

    static const elib_drv_dt_node_t roots[] = { ... };

It also accepts positional initializers (same field order as
elib_drv_dt_node_t). Point --root at the top-level array(s); without --root it
auto-selects arrays that are never referenced by another node's .children.

Outputs a tree-shaped Markdown/JSON view (CSV uses the same fields in row
form), checks
duplicate names, missing controllers, controller cycles, child-count mismatches
and depth, and prints the suggested handle-array size. Standard library only;
no compiler needed.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable

FIELD_ORDER = [
    "name",
    "fops",
    "ctx",
    "controller",
    "flags",
    "lock_timeout_ms",
    "children",
    "child_count",
]

KNOWN_FLAGS = {
    "ELIB_DRV_DEV_F_EXCLUSIVE": 1,
}

MAX_DEPTH = 8
IDENT_RE = re.compile(r"^[A-Za-z_]\w*$")
ARRAY_REF_RE = re.compile(r"^&?\s*([A-Za-z_]\w*)\s*(?:\[\s*0\s*\])?$")
ARRAY_SIZE_RE = re.compile(r"^ELIB_DRV_ARRAY_SIZE\s*\(\s*([A-Za-z_]\w*)\s*\)$")
INT_RE = re.compile(r"^(0[xX][0-9a-fA-F]+|\d+)[uUlL]*$")
STRING_RE = re.compile(r'^"((?:\\.|[^"\\])*)"$')


@dataclass
class RawNode:
    fields: dict[str, str]
    source: str
    line: int


@dataclass
class ArrayDef:
    name: str
    nodes: list[RawNode]
    source: str


@dataclass
class NodeInfo:
    order: int
    depth: int
    name: str
    kind: str
    controller_cfg: str
    controller_resolved: str
    parent: str
    fops: str
    ctx: str
    flags: str
    lock: str
    source: str
    path: str = ""
    root_array: str = ""
    raw_fields: dict[str, str] = field(default_factory=dict)
    child_nodes: list["NodeInfo"] = field(default_factory=list)


@dataclass
class TreeReport:
    roots: list[str]
    nodes: list[NodeInfo] = field(default_factory=list)
    root_nodes: list[NodeInfo] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)

    @property
    def node_count(self) -> int:
        return len(self.nodes)

    @property
    def max_depth(self) -> int:
        return max((n.depth for n in self.nodes), default=0)


def fail(message: str) -> None:
    print(f"dt_check: {message}", file=sys.stderr)
    raise SystemExit(2)


def mask_comments(text: str) -> str:
    """Replace C comments with spaces while preserving strings and newlines."""
    out = list(text)
    i = 0
    n = len(text)
    state = "normal"
    quote = ""
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "normal":
            if c == "/" and nxt == "/":
                out[i] = out[i + 1] = " "
                i += 2
                state = "line"
                continue
            if c == "/" and nxt == "*":
                out[i] = out[i + 1] = " "
                i += 2
                state = "block"
                continue
            if c in "\"'":
                state = "string"
                quote = c
            i += 1
            continue
        if state == "line":
            if c == "\n":
                state = "normal"
            else:
                out[i] = " "
            i += 1
            continue
        if state == "block":
            if c == "*" and nxt == "/":
                out[i] = out[i + 1] = " "
                i += 2
                state = "normal"
                continue
            if c != "\n":
                out[i] = " "
            i += 1
            continue
        # string / char literal
        if c == "\\":
            i += 2
            continue
        if c == quote:
            state = "normal"
        i += 1
    return "".join(out)


def find_matching(text: str, start: int, open_ch: str, close_ch: str) -> int:
    depth = 0
    i = start
    n = len(text)
    quote = ""
    while i < n:
        c = text[i]
        if quote:
            if c == "\\":
                i += 2
                continue
            if c == quote:
                quote = ""
            i += 1
            continue
        if c in "\"'":
            quote = c
            i += 1
            continue
        if c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def split_top_level(text: str, sep: str = ",") -> list[str]:
    parts: list[str] = []
    start = 0
    depth = 0
    i = 0
    n = len(text)
    quote = ""
    while i < n:
        c = text[i]
        if quote:
            if c == "\\":
                i += 2
                continue
            if c == quote:
                quote = ""
            i += 1
            continue
        if c in "\"'":
            quote = c
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == sep and depth == 0:
            parts.append(text[start:i])
            start = i + 1
        i += 1
    parts.append(text[start:])
    return parts


def unquote(value: str) -> str | None:
    value = value.strip()
    m = STRING_RE.match(value)
    if not m:
        return None
    raw = m.group(1)
    out: list[str] = []
    i = 0
    while i < len(raw):
        c = raw[i]
        if c != "\\":
            out.append(c)
            i += 1
            continue
        i += 1
        if i >= len(raw):
            break
        esc = raw[i]
        i += 1
        if esc == "n":
            out.append("\n")
        elif esc == "r":
            out.append("\r")
        elif esc == "t":
            out.append("\t")
        elif esc in ('"', "\\", "'"):
            out.append(esc)
        elif esc == "x" and i + 1 < len(raw):
            out.append(chr(int(raw[i:i + 2], 16)))
            i += 2
        else:
            out.append(esc)
    return "".join(out)


def strip_cast(value: str) -> str:
    value = value.strip()
    while value.startswith("("):
        close = find_matching(value, 0, "(", ")")
        if close <= 0:
            break
        value = value[close + 1:].strip()
    return value


def parse_ref(value: str) -> str | None:
    value = strip_cast(value)
    if value in ("", "NULL", "0", "0u", "0U"):
        return None
    m = ARRAY_REF_RE.match(value)
    if m:
        return m.group(1)
    return value


def parseInt(value: str) -> int | None:
    value = strip_cast(value).strip()
    m = INT_RE.match(value)
    if not m:
        return None
    return int(m.group(1), 0)


def parseFlags(value: str) -> str:
    value = strip_cast(value).strip()
    if value in ("", "0", "0u", "0U", "NULL"):
        return "-"
    total = 0
    ok = True
    for token in value.split("|"):
        token = strip_cast(token.strip())
        if token in KNOWN_FLAGS:
            total |= KNOWN_FLAGS[token]
        else:
            parsed = parseInt(token)
            if parsed is None:
                ok = False
                break
            total |= parsed
    if not ok:
        return value
    if total == 0:
        return "-"
    names = [name for name, bit in KNOWN_FLAGS.items() if total & bit]
    known = 0
    for name, bit in KNOWN_FLAGS.items():
        if total & bit:
            known |= bit
    if known == total:
        return "|".join(names)
    return f"{value} (0x{total:x})"


def parse_entry(entry: str) -> dict[str, str]:
    entry = entry.strip()
    if not (entry.startswith("{") and entry.endswith("}")):
        raise ValueError(f"initializer is not a braced node: {entry[:60]}")
    body = entry[1:-1]
    fields: dict[str, str] = {}
    positional = 0
    for part in split_top_level(body, ","):
        part = part.strip()
        if not part:
            continue
        m = re.match(r"^\.\s*([A-Za-z_]\w*)\s*=\s*(.*)$", part, re.S)
        if m:
            name = m.group(1)
            if name in fields:
                raise ValueError(f"duplicate .{name}")
            fields[name] = m.group(2).strip()
        else:
            if positional >= len(FIELD_ORDER):
                raise ValueError(f"too many positional fields: {part[:40]}")
            fields[FIELD_ORDER[positional]] = part
            positional += 1
    unknown = [k for k in fields if k not in FIELD_ORDER]
    if unknown:
        raise ValueError(f"unknown field(s): {', '.join(unknown)}")
    return fields


def parse_array(name: str, body: str, source: str, base_line: int) -> ArrayDef:
    nodes: list[RawNode] = []
    entry_offset = 0
    for part in split_top_level(body, ","):
        stripped = part.strip()
        if stripped:
            try:
                fields = parse_entry(stripped)
            except ValueError as exc:
                raise ValueError(f"{source}:{base_line}: {exc}") from exc
            # Approximate source line: base line + newlines before the entry.
            offset = body.find(stripped, entry_offset)
            if offset < 0:
                offset = entry_offset
            line = base_line + body.count("\n", 0, offset)
            nodes.append(RawNode(fields, source, line))
            entry_offset = offset + len(stripped)
    return ArrayDef(name, nodes, source)


def parse_sources(paths: Iterable[Path]) -> dict[str, ArrayDef]:
    arrays: dict[str, ArrayDef] = {}
    decl_re = re.compile(
        r"\belib_drv_dt_node_t\s+(?:(?:const|volatile)\s+)*"
        r"([A-Za-z_]\w*)\s*\[\s*[^\]]*\s*\]\s*=\s*\{"
    )
    for path in paths:
        try:
            text = path.read_text(encoding="utf-8")
        except OSError as exc:
            fail(f"cannot read {path}: {exc}")
        masked = mask_comments(text)
        for m in decl_re.finditer(masked):
            name = m.group(1)
            if name in arrays:
                fail(f"duplicate node array '{name}' (also in {arrays[name].source})")
            open_pos = masked.rfind("{", m.start(), m.end())
            if open_pos < 0:
                fail(f"{path}: cannot locate '{{' for {name}")
            close_pos = find_matching(masked, open_pos, "{", "}")
            if close_pos < 0:
                fail(f"{path}: unterminated array {name}")
            base_line = text.count("\n", 0, m.start()) + 1
            try:
                arrays[name] = parse_array(
                    name, masked[open_pos + 1:close_pos], str(path), base_line
                )
            except ValueError as exc:
                fail(str(exc))
    return arrays


def array_ref(fields: dict[str, str]) -> str | None:
    return parse_ref(fields.get("children", ""))


def child_count_for(
    raw: RawNode, arrays: dict[str, ArrayDef], errors: list[str], warnings: list[str]
) -> int:
    child_ref = array_ref(raw.fields)
    declared = raw.fields.get("child_count", "").strip()
    actual = len(arrays[child_ref].nodes) if child_ref in arrays else None

    if child_ref is not None and actual is None:
        errors.append(
            f"{raw.source}:{raw.line}: children array '{child_ref}' is not declared"
        )
        return 0

    if declared == "":
        return actual if actual is not None else 0

    m = ARRAY_SIZE_RE.match(strip_cast(declared))
    if m:
        ref = m.group(1)
        if ref in arrays:
            actual = len(arrays[ref].nodes)
        else:
            warnings.append(
                f"{raw.source}:{raw.line}: child_count references unknown array '{ref}'"
            )
            return actual if actual is not None else 0
    else:
        parsed = parseInt(declared)
        if parsed is None:
            warnings.append(
                f"{raw.source}:{raw.line}: cannot evaluate child_count '{declared}'"
            )
            return actual if actual is not None else 0
        actual = parsed

    if child_ref is None:
        if actual != 0:
            errors.append(
                f"{raw.source}:{raw.line}: child_count={actual} but children is NULL"
            )
        return actual

    if actual != len(arrays[child_ref].nodes):
        errors.append(
            f"{raw.source}:{raw.line}: child_count={actual} but "
            f"'{child_ref}' has {len(arrays[child_ref].nodes)} entries"
        )
    return actual


def controller_value(fields: dict[str, str]) -> str | None:
    raw = fields.get("controller", "").strip()
    if raw == "" or raw in ("NULL", "0", "0u", "0U"):
        return None
    value = unquote(raw)
    if value is not None:
        return value
    return strip_cast(raw)


def name_value(raw: RawNode) -> str | None:
    value = raw.fields.get("name", "").strip()
    if value in ("", "NULL"):
        return None
    return unquote(value)


def build_report(
    arrays: dict[str, ArrayDef],
    roots: list[str],
    capacity: int | None,
) -> TreeReport:
    report = TreeReport(roots=roots)
    by_name: dict[str, NodeInfo] = {}
    used_arrays: set[str] = set()
    active_arrays: set[str] = set()
    order = 0

    def walk(
        array_name: str,
        parent: str,
        depth: int,
        parent_info: NodeInfo | None,
        parent_path: str,
        root_array: str,
    ) -> None:
        nonlocal order
        if array_name in active_arrays:
            report.errors.append(f"children cycle through array '{array_name}'")
            return
        if array_name in used_arrays:
            report.errors.append(f"array '{array_name}' is referenced more than once")
            return
        active_arrays.add(array_name)
        used_arrays.add(array_name)

        array = arrays[array_name]
        for raw in array.nodes:
            name = name_value(raw)
            if name is None:
                report.errors.append(
                    f"{raw.source}:{raw.line}: node has no string .name"
                )
                name = f"<unnamed@{raw.source}:{raw.line}>"
            if name in by_name:
                report.errors.append(
                    f"{raw.source}:{raw.line}: duplicate node name '{name}' "
                    f"(first at {by_name[name].source})"
                )

            child_ref = array_ref(raw.fields)
            count = child_count_for(raw, arrays, report.errors, report.warnings)
            cfg_ctrl = controller_value(raw.fields)
            resolved = cfg_ctrl if cfg_ctrl is not None else (parent or None)

            if depth > MAX_DEPTH:
                report.errors.append(
                    f"{raw.source}:{raw.line}: '{name}' depth {depth} exceeds "
                    f"ELIB_DRV_DT_MAX_DEPTH={MAX_DEPTH}"
                )

            if child_ref is not None and count > 0:
                kind = "controller"
            elif resolved is not None:
                kind = "device"
            else:
                kind = "node"

            info = NodeInfo(
                order=order,
                depth=depth,
                name=name,
                kind=kind,
                controller_cfg=cfg_ctrl or "-",
                controller_resolved=resolved or "-",
                parent=parent or "-",
                fops=strip_cast(raw.fields.get("fops", "")) or "-",
                ctx=strip_cast(raw.fields.get("ctx", "")) or "-",
                flags=parseFlags(raw.fields.get("flags", "")),
                lock=raw.fields.get("lock_timeout_ms", "").strip() or "0",
                source=f"{raw.source}:{raw.line}",
                path=f"{parent_path}/{name}" if parent_path else name,
                root_array=root_array,
                raw_fields=dict(raw.fields),
            )
            order += 1
            report.nodes.append(info)
            if parent_info is None:
                report.root_nodes.append(info)
            else:
                parent_info.child_nodes.append(info)
            by_name.setdefault(name, info)

            if child_ref is not None:
                if child_ref not in arrays:
                    report.errors.append(
                        f"{raw.source}:{raw.line}: children array "
                        f"'{child_ref}' is not declared"
                    )
                elif count > 0:
                    walk(child_ref, name, depth + 1, info, info.path, root_array)

        active_arrays.remove(array_name)

    for root in roots:
        if root not in arrays:
            report.errors.append(f"root array '{root}' is not declared")
            continue
        walk(root, "", 1, None, root, root)

    if not report.nodes and not report.errors:
        report.errors.append("no nodes reachable from the selected roots")

    # Explicit controller references must point to a reachable node.
    for node in report.nodes:
        if node.controller_cfg == "-":
            continue
        if node.controller_cfg not in by_name:
            report.errors.append(
                f"{node.source}: '{node.name}' references missing controller "
                f"'{node.controller_cfg}'"
            )
        elif node.controller_cfg == node.name:
            report.errors.append(
                f"{node.source}: '{node.name}' references itself as controller"
            )

    # Detect controller-chain cycles.
    for node in report.nodes:
        seen: set[str] = set()
        current = node
        while current.controller_resolved != "-":
            if current.name in seen:
                report.errors.append(
                    f"{node.source}: controller chain from '{node.name}' cycles"
                )
                break
            seen.add(current.name)
            next_node = by_name.get(current.controller_resolved)
            if next_node is None:
                break
            current = next_node

    if capacity is not None and report.node_count > capacity:
        report.errors.append(
            f"node count {report.node_count} exceeds handle capacity {capacity}"
        )

    return report


def raw_field(node: NodeInfo, name: str) -> str | None:
    value = node.raw_fields.get(name, "").strip()
    if value == "" or value == "NULL":
        return None
    return strip_cast(value)


def csv_rows(report: TreeReport) -> list[list[str]]:
    rows = []
    for n in report.nodes:
        ctrl = controller_value(n.raw_fields)
        rows.append([
            n.name,
            raw_field(n, "fops") or "",
            raw_field(n, "ctx") or "",
            ctrl or "",
            raw_field(n, "flags") or "0",
            raw_field(n, "lock_timeout_ms") or "0",
            raw_field(n, "child_count") or "0",
        ])
    return rows


def render_tree_node(
    node: NodeInfo, prefix: str, last: bool, lines: list[str]
) -> None:
    connector = "`- " if last else "+- "
    parts = [node.name]
    fields = (
        ("fops", raw_field(node, "fops")),
        ("ctx", raw_field(node, "ctx")),
        ("controller", controller_value(node.raw_fields)),
        ("flags", raw_field(node, "flags")),
        ("lock_timeout_ms", raw_field(node, "lock_timeout_ms")),
        ("child_count", raw_field(node, "child_count")),
    )
    for key, value in fields:
        if value is not None:
            parts.append(f"{key}={value}")
    lines.append(prefix + connector + " ".join(parts))
    child_prefix = prefix + ("   " if last else "|  ")
    for i, child in enumerate(node.child_nodes):
        render_tree_node(child, child_prefix, i == len(node.child_nodes) - 1,
                         lines)


def print_tree(report: TreeReport) -> None:
    lines: list[str] = []
    for root in report.roots:
        roots_here = [n for n in report.root_nodes if n.root_array == root]
        if not roots_here:
            continue
        lines.append(root)
        for i, node in enumerate(roots_here):
            render_tree_node(node, "", i == len(roots_here) - 1, lines)
    print("\n".join(lines))


def print_diagnostics(report: TreeReport, capacity: int | None) -> None:
    print(f"Root arrays: {', '.join(report.roots)}", file=sys.stderr)
    print(f"Node count: {report.node_count}", file=sys.stderr)
    print(f"Max depth: {report.max_depth} (limit {MAX_DEPTH})", file=sys.stderr)
    print(f"Suggested handle array: elib_drv_node_t nodes[{report.node_count}];",
          file=sys.stderr)
    if capacity is not None:
        print(f"Capacity check: {report.node_count} <= {capacity}",
              file=sys.stderr)
    if report.errors:
        print(f"Errors: {len(report.errors)}", file=sys.stderr)
        for err in report.errors:
            print(f"  - {err}", file=sys.stderr)
    if report.warnings:
        print(f"Warnings: {len(report.warnings)}", file=sys.stderr)
        for warn in report.warnings:
            print(f"  - {warn}", file=sys.stderr)


def print_csv(report: TreeReport) -> None:
    writer = csv.writer(sys.stdout, lineterminator="\n")
    writer.writerow([
        "name", "fops", "ctx", "controller", "flags",
        "lock_timeout_ms", "child_count",
    ])
    writer.writerows(csv_rows(report))


def node_json(node: NodeInfo) -> dict[str, Any]:
    return {
        "name": node.name,
        "fops": raw_field(node, "fops"),
        "ctx": raw_field(node, "ctx"),
        "controller": controller_value(node.raw_fields),
        "flags": raw_field(node, "flags") or "0",
        "lock_timeout_ms": raw_field(node, "lock_timeout_ms") or "0",
        "child_count": raw_field(node, "child_count") or "0",
        "children": [node_json(child) for child in node.child_nodes],
    }


def print_json(report: TreeReport) -> None:
    json.dump([node_json(n) for n in report.root_nodes], sys.stdout,
              indent=2, ensure_ascii=False)
    print()


def collect_files(paths: list[str]) -> list[Path]:
    result: list[Path] = []
    for item in paths:
        path = Path(item)
        if path.is_dir():
            result.extend(sorted(
                p for p in path.rglob("*")
                if p.suffix.lower() in (".c", ".h")
            ))
        elif path.is_file():
            result.append(path)
        else:
            fail(f"path does not exist: {item}")
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Parse C elib_drv_dt_node_t arrays into a review tree."
    )
    parser.add_argument("files", nargs="+", help="C source/header files or directories")
    parser.add_argument(
        "--root", action="append", default=[],
        help="top-level dt node array; repeat or comma-separate",
    )
    parser.add_argument(
        "--capacity", type=int, default=None,
        help="check against this elib_drv_node_t handle capacity",
    )
    parser.add_argument(
        "--format", choices=("md", "csv", "json"), default="md",
        help="output format (default: md)",
    )
    parser.add_argument(
        "--strict", action="store_true",
        help="treat warnings as errors",
    )
    args = parser.parse_args(argv)

    files = collect_files(args.files)
    if not files:
        fail("no .c/.h files found")

    arrays = parse_sources(files)
    if not arrays:
        fail("no elib_drv_dt_node_t arrays found")

    roots: list[str] = []
    for item in args.root:
        roots.extend(part.strip() for part in item.split(",") if part.strip())
    if not roots:
        referenced = set()
        for array in arrays.values():
            for raw in array.nodes:
                ref = array_ref(raw.fields)
                if ref is not None:
                    referenced.add(ref)
        roots = [name for name in arrays if name not in referenced]
    roots = list(dict.fromkeys(roots))
    if not roots:
        fail("no root array found; pass --root <array>")

    report = build_report(arrays, roots, args.capacity)

    if args.format == "markdown":
        args.format = "md"
    if args.format == "md":
        print_tree(report)
    elif args.format == "csv":
        print_csv(report)
    else:
        print_json(report)

    print_diagnostics(report, args.capacity)

    if report.errors:
        return 1
    if args.strict and report.warnings:
        return 1
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BrokenPipeError:
        raise SystemExit(0)
