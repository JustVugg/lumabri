#!/usr/bin/env python3
"""Build-time-only embedding of the public management contracts for MCP."""
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
documents = (
    ("resident-api", "Resident inference and allocation API", "RESIDENT_API.md"),
    ("preparation", "Model preparation and durable operations", "MODEL_PREPARATION_API.md"),
    ("elasticity", "Bounded replica policy", "ELASTIC_REPLICAS.md"),
    ("portfolio", "Measured portfolio policy", "PORTFOLIO_POLICY.md"),
    ("donor-authorization", "Donor authorization", "DONOR_AUTHORIZATION.md"),
)
print("static const struct { const char *uri, *name, *text; } agent_docs[] = {")
for key, title, filename in documents:
    print("{" + ",".join(json.dumps(s) for s in (
        "lumabri://docs/" + key, title, (root / "docs" / filename).read_text())) + "},")
print("};")
