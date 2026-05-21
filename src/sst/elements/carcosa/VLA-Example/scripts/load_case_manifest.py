#!/usr/bin/env python3
"""Load case_studies/manifest.yaml (minimal parser, no PyYAML required)."""
from __future__ import annotations

import os
import sys
from typing import Any, Dict


def _parse_simple_yaml(path: str) -> Dict[str, Any]:
    """Parse the flat manifest structure we emit by hand."""
    root: Dict[str, Any] = {}
    stack: list = [root]
    keys_stack: list = []

    def current_dict():
        return stack[-1]

    with open(path, "r") as f:
        for raw in f:
            line = raw.split("#", 1)[0].rstrip()
            if not line.strip():
                continue
            indent = len(line) - len(line.lstrip())
            key, _, rest = line.strip().partition(":")
            val = rest.strip()

            while len(keys_stack) > 0 and indent <= keys_stack[-1][0]:
                keys_stack.pop()
                stack.pop()

            if val == "":
                new: Dict[str, Any] = {}
                current_dict()[key] = new
                keys_stack.append((indent, key))
                stack.append(new)
            else:
                if val.lower() in ("true", "false"):
                    current_dict()[key] = val.lower() == "true"
                else:
                    try:
                        if "." in val or "e" in val.lower():
                            current_dict()[key] = float(val)
                        else:
                            current_dict()[key] = int(val)
                    except ValueError:
                        current_dict()[key] = val
    return root


def load_manifest() -> Dict[str, Any]:
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "..", "case_studies", "manifest.yaml")
    if not os.path.isfile(path):
        sys.stderr.write(f"ERROR: manifest not found: {path}\n")
        sys.exit(1)
    return _parse_simple_yaml(path)


def emit_shell_defaults() -> None:
    m = load_manifest()
    d = m.get("defaults", {})
    print(f"export VLA_MAX_CYCLES={d.get('vla_max_cycles', 8)}")
    print(f"export ECC_CAMPAIGN_TARGET_KERNEL={d.get('campaign_target_kernel', 'ACTUATE')}")
    print(f"export ECC_ADDR_FILTER_REGION={d.get('addr_filter_region', 'action_queue')}")
    print(f"export ECC_ADDR_FILTER_LEN={d.get('addr_filter_len', 64)}")
    print(f"export ECC_CAMPAIGN_MAX_PER_KERNEL_ENTRY={d.get('campaign_max_events_per_kernel_entry', 1)}")
    print(f"CASE_DEFAULT_BUDGET={d.get('campaign_event_budget', 8)}")
    print(f"CASE_DEFAULT_RATE={d.get('campaign_event_rate', 1.0)}")


def case_env(case_id: str) -> Dict[str, Any]:
    m = load_manifest()
    c = m.get("cases", {}).get(case_id, {})
    d = m.get("defaults", {})
    return {
        "mode": c.get("campaign_mode", "cell"),
        "budget": c.get("campaign_event_budget", d.get("campaign_event_budget", 8)),
        "fixed": c.get("campaign_errors_fixed", 0),
        "multi": c.get("campaign_force_multi_chip", False),
    }


if __name__ == "__main__":
    import sys
    if len(sys.argv) > 1 and sys.argv[1] == "--shell-defaults":
        emit_shell_defaults()
    elif len(sys.argv) > 2 and sys.argv[1] == "--case":
        import json
        print(json.dumps(case_env(sys.argv[2])))
    else:
        import json
        print(json.dumps(load_manifest(), indent=2))
