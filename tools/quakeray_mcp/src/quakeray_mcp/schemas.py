def object_schema(properties, required=()):
    return {"type": "object", "properties": properties, "required": list(required), "additionalProperties": False}


def integer(maximum, default=0):
    return {"type": "integer", "minimum": 0, "maximum": maximum, "default": default}


SOURCE = {"type": "object", "oneOf": [
    object_schema({"import_path": {"type": "string", "minLength": 1, "maxLength": 2048}}, ("import_path",)),
    object_schema({"artifact_id": {"type": "string", "minLength": 1, "maxLength": 100}}, ("artifact_id",)),
]}

INPUTS = {
    "server_status": object_schema({}),
    "list_scenarios": object_schema({"runtime_id": {"type": "string", "enum": ["debug"]},
                                     "kind": {"type": "string", "enum": ["stress"]}}),
    "read_stats": object_schema({"source": SOURCE, "summary": {"type": "boolean", "default": True},
                                  "cursor": integer(32768), "limit": {**integer(100, 20), "minimum": 1},
                                  "budget_ms": {"type": "number", "exclusiveMinimum": 0, "maximum": 10000,
                                                "default": 1000 / 60}}, ("source",)),
    "read_benchmarks": object_schema({"source": SOURCE, "last_n": {**integer(20, 5), "minimum": 1}}, ("source",)),
    "compare_runs": object_schema({"baseline": SOURCE, "candidate": SOURCE,
                                    "policy_id": {"type": "string", "enum": ["diagnostic"]}},
                                   ("baseline", "candidate")),
    "get_architecture": object_schema({"query": {"type": "string", "maxLength": 256},
                                        "document": {"type": "string", "maxLength": 256},
                                        "cursor": integer(2097152)}),
}

REQUEST_KEY = {"type": "string", "minLength": 16, "maxLength": 128, "pattern": "^[A-Za-z0-9_-]+$"}
RUNTIME_ID = {"type": "string", "enum": ["debug"]}
SCENARIO_ID = {"type": "string", "enum": [f"{save}-{preset}" for save in ("qr_fuma_start", "qr_ad_start", "qr_gpu_heavy") for preset in ("balanced", "quality")]}
JOB_ID = {"type": "string", "pattern": "^job_[a-f0-9]{32}$"}
JOB_INPUTS = {
    "start_build": object_schema({"runtime_id": RUNTIME_ID, "idempotency_key": REQUEST_KEY,
                                   "config": {"const": "Debug", "default": "Debug"},
                                   "tests": {"type": "boolean", "default": True},
                                   "parallel": {**integer(32, 4), "minimum": 1}}, ("runtime_id", "idempotency_key")),
    "start_benchmark": object_schema({"scenario_id": SCENARIO_ID, "runtime_id": RUNTIME_ID,
                                       "idempotency_key": REQUEST_KEY, "warmup_s": {**integer(30, 8), "minimum": 2},
                                       "seconds": {**integer(25, 6), "minimum": 2}, "repeats": {"const": 1, "default": 1}},
                                      ("scenario_id", "runtime_id", "idempotency_key")),
    "start_capture": object_schema({"scenario_id": SCENARIO_ID, "runtime_id": RUNTIME_ID,
                                     "idempotency_key": REQUEST_KEY, "warmup_s": {**integer(30, 8), "minimum": 2},
                                     "duration_s": {**integer(25, 6), "minimum": 2}},
                                    ("scenario_id", "runtime_id", "idempotency_key")),
    "start_menu_ab": object_schema({"candidate_runtime": RUNTIME_ID, "idempotency_key": REQUEST_KEY,
                                     "seconds": {**integer(25, 8), "minimum": 2}, "smoke": {"type": "boolean", "default": False},
                                     "validation": {"type": "boolean", "default": False}}, ("candidate_runtime", "idempotency_key")),
    "get_job": object_schema({"job_id": JOB_ID, "wait_seconds": integer(10)}, ("job_id",)),
    "cancel_job": object_schema({"job_id": JOB_ID, "control_token": {"type": "string", "minLength": 16, "maxLength": 128}},
                                 ("job_id", "control_token")),
    "list_runs": object_schema({"last_n": {**integer(50, 20), "minimum": 1}}),
}
ALL_INPUTS = {**INPUTS, **JOB_INPUTS}

DESCRIPTIONS = {
    "server_status": "Inspect source, runtime readiness and processes without building or launching the game.",
    "list_scenarios": "List documented Fuma, AD-hub and Bogbottom scenarios and missing prerequisites.",
    "read_stats": "Read an approved frame CSV, window dump or one-shot snapshot with explicit aggregation and provenance gaps.",
    "read_benchmarks": "Inspect benchmark blocks and their exact frame_samples associations; no automatic baseline creation.",
    "compare_runs": "Compare imported metrics diagnostically. Unverified provenance never produces an acceptance PASS.",
    "get_architecture": "Read AGENTS.md, ARCHITECTURE.md, PERFORMANCE.md or an allowlisted documentation section.",
}
DESCRIPTIONS.update({
    "start_build": "Start an approved Debug build under shared ownership and Windows process containment; tests are compiled, not run.",
    "start_benchmark": "Run one registered save/preset in an isolated runtime sandbox with a 300-second deadline.",
    "start_capture": "Capture diagnostic frame/window data with StatsLevel 3 in a private runtime sandbox.",
    "start_menu_ab": "Capture the registered candidate menu/smoke profile in a sandbox; baseline runtime registry is pending.",
    "get_job": "Inspect a persistent job and bounded untrusted log tails; waiting never resets the deadline.",
    "cancel_job": "Terminate only the owned process tree using the creator control capability; no arbitrary PID.",
    "list_runs": "List persistent completed capture records without granting performance or visual acceptance.",
})

REQUIRED_OUTPUTS = {
    "server_status": ("phase", "source", "runtime", "processes", "capabilities"),
    "list_scenarios": ("runtime_id", "scenarios", "evidence_document"),
    "read_stats": ("artifact", "format", "aggregation_kind", "metadata", "page", "validity"),
    "read_benchmarks": ("artifact", "blocks", "total_blocks"),
    "compare_runs": ("comparability", "deltas", "performance", "visual"),
    "get_architecture": ("documents", "source", "untrusted_data"),
}

DATA_PROPERTIES = {
    "phase": {"type": "string"},
    "source": {"type": "object"},
    "runtime": {"type": "object"},
    "processes": {"type": "object"},
    "capabilities": {"type": "object"},
    "runtime_id": {"type": "string"},
    "scenarios": {"type": "array", "items": {"type": "object"}, "maxItems": 100},
    "evidence_document": {"type": "string"},
    "artifact": {"type": "object", "required": ["artifact_id", "sha256", "byte_count", "origin"]},
    "format": {"enum": ["frame_csv", "window_csv", "snapshot"]},
    "aggregation_kind": {"enum": ["per_frame", "reporting_window", "readout_snapshot"]},
    "metadata": {"type": "object"},
    "page": {"type": "object", "required": ["kind", "cursor", "items", "total", "next_cursor"],
             "properties": {"kind": {"enum": ["rows", "metrics"]}, "cursor": integer(32768),
                            "items": {"type": "array", "items": {"type": "object"}, "maxItems": 100},
                            "total": {"type": "integer", "minimum": 0},
                            "next_cursor": {"type": ["integer", "null"], "minimum": 0}}},
    "validity": {"type": "object"},
    "blocks": {"type": "array", "items": {"type": "object"}, "maxItems": 20},
    "total_blocks": {"type": "integer", "minimum": 0},
    "comparability": {"enum": ["compatible", "incomparable"]},
    "deltas": {"type": "array", "items": {"type": "object"}},
    "performance": {"const": "not_checked"},
    "visual": {"const": "not_checked"},
    "documents": {"type": "array", "items": {"type": "object"}},
    "untrusted_data": {"const": True},
}

OPTIONAL_OUTPUTS = {
    "read_stats": ("schema", "issues", "summary", "untrusted_data", "evidence_route"),
    "read_benchmarks": ("untrusted_data",),
    "compare_runs": ("reasons", "missing_metrics", "gameplay", "validation", "policy", "baseline_artifact", "candidate_artifact"),
}


def output_schema(name):
    error = {"type": "object", "required": ["code", "message", "remediation", "retryable"],
             "properties": {"code": {"type": "string"}, "message": {"type": "string"},
                            "remediation": {"type": "string"}, "retryable": {"type": "boolean"}},
             "additionalProperties": False}
    if name in JOB_INPUTS:
        return {"type": "object", "required": ["schema_version", "ok", "data", "error"],
                "properties": {"schema_version": {"const": 1}, "ok": {"type": "boolean"},
                               "data": {"type": ["object", "null"]}, "error": {"anyOf": [error, {"type": "null"}]}},
                "additionalProperties": False}
    keys = REQUIRED_OUTPUTS[name] + OPTIONAL_OUTPUTS.get(name, ())
    properties = {key: DATA_PROPERTIES.get(key, {"type": "object"}) for key in keys}
    for key in ("issues", "reasons", "missing_metrics"):
        if key in properties:
            properties[key] = {"type": "array", "items": {"type": "string"}}
    for key in ("gameplay", "validation", "policy"):
        if key in properties:
            properties[key] = {"type": "string"}
    data = {"type": "object", "required": list(REQUIRED_OUTPUTS[name]), "properties": properties,
            "additionalProperties": False}
    return {"type": "object", "required": ["schema_version", "ok", "data", "error"],
            "properties": {"schema_version": {"const": 1}, "ok": {"type": "boolean"},
                           "data": {"type": ["object", "null"]}, "error": {"anyOf": [error, {"type": "null"}]}},
            "additionalProperties": False,
            "oneOf": [{"properties": {"ok": {"const": True}, "error": {"type": "null"},
                                      "data": data}},
                      {"properties": {"ok": {"const": False}, "data": {"type": "null"}, "error": error}}]}
