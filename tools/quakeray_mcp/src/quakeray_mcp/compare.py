import math

from .errors import EvidenceError


IDENTITY_FIELDS = (
    "scenario_id", "save_sha256", "map_assets_fingerprint", "mod_assets_sha256", "engine_assets_sha256",
    "profile", "effective_settings", "hardware", "driver", "build_config", "instrumentation",
)


def compare_records(baseline, candidate):
    reasons = []
    for side, record in (("baseline", baseline), ("candidate", candidate)):
        instrumentation = record.get('identity', {}).get('instrumentation', {})
        if isinstance(instrumentation, dict) and instrumentation.get('stats_level', 0) != 0 or record.get('identity', {}).get('profile') == 'diagnostic_instrumentation':
            reasons.append(f'{side}: diagnostic instrumentation is not a target-performance control')
        if record.get("validity") != "valid":
            reasons.append(f"{side}: measurement validity not established")
        if record.get("provenance_verified") is not True:
            reasons.append(f"{side}: provenance not verified")
    for key in IDENTITY_FIELDS:
        a, b = baseline.get("identity", {}).get(key), candidate.get("identity", {}).get(key)
        if a is None or b is None or a in ("", {}, []) or b in ("", {}, []):
            reasons.append(f"Missing identity: {key}")
        elif a != b:
            reasons.append(f"Incompatible identity: {key}")
    if baseline.get("aggregation_kind") != candidate.get("aggregation_kind"):
        reasons.append("Incompatible aggregation kinds")
    if baseline.get("map") != candidate.get("map"):
        reasons.append("Incompatible map")
    base_metrics, cand_metrics = baseline.get("metrics", {}), candidate.get("metrics", {})
    missing = sorted(set(base_metrics) ^ set(cand_metrics))
    if missing:
        reasons.append("Metric coverage differs")
    deltas = []
    for name in sorted(set(base_metrics) & set(cand_metrics)):
        a, b = base_metrics[name], cand_metrics[name]
        if a is None or b is None:
            reasons.append(f"Unavailable metric: {name}")
            continue
        if not all(isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v) for v in (a, b)):
            raise EvidenceError("PARSE_FAILED", "Comparison metrics must be finite numbers")
        direction = "diagnostic"
        if name == "fps":
            direction = "higher_is_better"
        elif name.startswith(("cpu.", "gpu.")) and name.endswith("_ms") or name in {
            "interval_ms", "host_interval_ms", "mean_ms", "p50_ms", "p90_ms", "p95_ms", "p99_ms", "max_ms"
        }:
            direction = "lower_is_better"
        delta = b - a
        percent = (b / a - 1) * 100 if a != 0 else None
        if not math.isfinite(delta) or percent is not None and not math.isfinite(percent):
            raise EvidenceError("PARSE_FAILED", "Comparison arithmetic overflowed")
        deltas.append({"name": name, "baseline": a, "candidate": b, "delta": delta,
                       "delta_percent": percent,
                       "direction": direction})
    if not deltas:
        reasons.append("No common measured metrics")
    performance = 'not_checked'
    if not reasons:
        timings = [row for row in deltas if row['name'] in {'mean_ms', 'p95_ms'}]
        if len(timings) == 2:
            performance = 'regression' if any(row['candidate'] > row['baseline'] * 1.05 for row in timings) else 'within_5pct_tolerance'
    return {"comparability": "incomparable" if reasons else "compatible",
            "reasons": reasons, "missing_metrics": missing, "deltas": deltas,
            "performance": performance, "visual": "not_checked", "gameplay": "not_checked",
            "validation": "not_checked", "policy": "matched_mean_p95_5pct" if not reasons else "diagnostic_no_acceptance"}
