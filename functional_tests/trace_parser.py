"""Trace parser for envy structured trace JSONL files.

Parses JSONL trace files emitted by envy --trace=file:path.jsonl and provides
filtering and analysis helpers for test assertions.

Schema v2: the first record is a trace_start header carrying the schema version.
Every record carries envelope keys: seq (monotonic, causal order), ts (emit-time
ISO8601), tid (small sequential thread id), event, and spec (subject package
identity; omitted for engine-scoped events).

EVENT_REGISTRY and SCHEMA_VERSION are read from src/trace_events.def and
src/trace.h, the sources the binary compiles.
"""

import json
import re
from dataclasses import dataclass
from enum import IntEnum
from pathlib import Path
from typing import Callable, List, Optional

_SRC = Path(__file__).resolve().parent.parent / "src"


def _load_registry() -> dict[str, list[str]]:
    """event name -> field names, in declaration order."""
    text = (_SRC / "trace_events.def").read_text(encoding="utf-8")
    text = re.sub(r"//[^\n]*", "", text)
    registry: dict[str, list[str]] = {}
    fields: list[str] = []
    for m in re.finditer(
        r"ENVY_TRACE_EVENT\(\s*(\w+)|ENVY_TRACE_FIELD_\w+\(\s*(\w+)\s*\)", text
    ):
        if m.group(1):
            fields = registry.setdefault(m.group(1), [])
        else:
            fields.append(m.group(2))
    if "trace_start" not in registry:
        raise RuntimeError(f"no trace_start event parsed from {_SRC / 'trace_events.def'}")
    return registry


EVENT_REGISTRY = _load_registry()
SCHEMA_VERSION = int(
    re.search(
        r"kTraceSchemaVersion\{\s*(\d+)\s*\}",
        (_SRC / "trace.h").read_text(encoding="utf-8"),
    ).group(1)
)


class PkgPhase(IntEnum):
    """Spec phase enum (matches src/pkg_phase.h)"""

    NONE = -1
    SPEC_FETCH = 0
    PKG_CHECK = 1
    PKG_IMPORT = 2
    PKG_FETCH = 3
    PKG_STAGE = 4
    PKG_BUILD = 5
    PKG_INSTALL = 6
    PKG_SETUP = 7
    PKG_EXPORT = 8
    PKG_VENDOR = 9
    COMPLETION = 10


# Serialized phase names (matches src/pkg_phase.cpp name table).
PHASE_BY_NAME = {
    "none": PkgPhase.NONE,
    "spec_fetch": PkgPhase.SPEC_FETCH,
    "check": PkgPhase.PKG_CHECK,
    "import": PkgPhase.PKG_IMPORT,
    "fetch": PkgPhase.PKG_FETCH,
    "stage": PkgPhase.PKG_STAGE,
    "build": PkgPhase.PKG_BUILD,
    "install": PkgPhase.PKG_INSTALL,
    "setup": PkgPhase.PKG_SETUP,
    "export": PkgPhase.PKG_EXPORT,
    "vendor": PkgPhase.PKG_VENDOR,
    "completion": PkgPhase.COMPLETION,
}


def parse_phase(name: str) -> PkgPhase:
    """Map a serialized phase name to PkgPhase; raises on unknown names."""
    if name not in PHASE_BY_NAME:
        raise ValueError(f"Unknown phase name: {name!r}")
    return PHASE_BY_NAME[name]


@dataclass
class TraceEvent:
    """Parsed trace event with envelope fields."""

    event: str
    seq: int
    ts: str
    tid: int
    spec: Optional[str]
    raw: dict  # Full JSON for accessing event-specific fields

    @property
    def phase(self) -> Optional[PkgPhase]:
        """Get phase from event (if present), returns enum value."""
        for key in ("phase", "blocked_at_phase", "needed_by", "target_phase"):
            if key in self.raw:
                return parse_phase(self.raw[key])
        return None


class TraceParser:
    """Parser for envy trace JSONL files (schema v2, strict)."""

    def __init__(self, trace_file: Path):
        """Initialize parser with trace file path."""
        self.trace_file = Path(trace_file)
        self._events: Optional[List[TraceEvent]] = None

    def parse(self) -> List[TraceEvent]:
        """Parse all events, validate the header, and sort by seq."""
        if self._events is not None:
            return self._events

        events = []
        with open(self.trace_file) as f:
            for line_num, line in enumerate(f, 1):
                line = line.strip()
                if not line:
                    continue

                try:
                    data = json.loads(line)
                    for key in ("event", "seq", "ts", "tid"):
                        if key not in data:
                            raise ValueError(f"Missing envelope key {key!r}")
                    name = data["event"]
                    if name not in EVENT_REGISTRY:
                        raise ValueError(f"Unknown event type: {name!r}")
                    events.append(
                        TraceEvent(
                            event=name,
                            seq=data["seq"],
                            ts=data["ts"],
                            tid=data["tid"],
                            spec=data.get("spec"),
                            raw=data,
                        )
                    )
                except (json.JSONDecodeError, ValueError) as e:
                    raise ValueError(f"Invalid trace line {line_num}: {e}") from e

        events.sort(key=lambda e: e.seq)

        if not events or events[0].event != "trace_start":
            raise ValueError("Trace stream missing trace_start header record")
        schema = events[0].raw.get("schema")
        if schema != SCHEMA_VERSION:
            raise ValueError(
                f"Trace schema mismatch: expected {SCHEMA_VERSION}, got {schema}"
            )

        self._events = events
        return events

    def filter_by_event(self, event_type: str) -> List[TraceEvent]:
        """Filter events by type; raises on names absent from the registry."""
        if event_type not in EVENT_REGISTRY:
            raise ValueError(f"Unknown event type: {event_type!r}")
        return [e for e in self.parse() if e.event == event_type]

    def filter_by_spec(self, spec: str) -> List[TraceEvent]:
        """Filter events by spec identity."""
        return [e for e in self.parse() if e.spec == spec]

    def filter_by_spec_and_event(self, spec: str, event_type: str) -> List[TraceEvent]:
        """Filter events by spec and event type."""
        return [e for e in self.filter_by_event(event_type) if e.spec == spec]

    def registered_specs(self) -> set:
        """Canonical keys of every spec the engine added to the dependency graph.

        Registration happens as the graph is built, before any phase runs, so this
        answers "what did resolution pull in", NOT "what succeeded" -- a spec whose
        FETCH throws still appears here. Assert on it for resolution shape, and pair
        it with the exit code (or pkg_outcome) when the test is about completion.
        """
        return {e.raw["key"] for e in self.filter_by_event("spec_registered")}

    def get_dependency_added_events(self, parent: str) -> List[TraceEvent]:
        """Get all dependency_added events for a given parent spec."""
        return self.filter_by_spec_and_event(parent, "dependency_added")

    def get_phase_sequence(self, spec: str) -> List[PkgPhase]:
        """Get the sequence of phases executed for a spec."""
        phase_starts = self.filter_by_spec_and_event(spec, "phase_start")
        return [parse_phase(e.raw["phase"]) for e in phase_starts]

    def assert_phase_sequence(self, spec: str, expected: List[PkgPhase]) -> None:
        """Assert that a spec executed phases in expected order."""
        actual = self.get_phase_sequence(spec)
        assert actual == expected, (
            f"Phase sequence mismatch for {spec}:\n"
            f"  Expected: {[p.name for p in expected]}\n"
            f"  Actual:   {[p.name for p in actual]}"
        )

    def assert_dependency_needed_by(
        self, parent: str, dependency: str, expected_phase: PkgPhase
    ) -> None:
        """Assert that a dependency has the expected needed_by phase."""
        deps = self.get_dependency_added_events(parent)
        matching = [e for e in deps if e.raw.get("dependency") == dependency]

        assert matching, f"No dependency_added event found for {parent} -> {dependency}"

        actual_phase = parse_phase(matching[0].raw["needed_by"])

        assert actual_phase == expected_phase, (
            f"Wrong needed_by phase for {parent} -> {dependency}:\n"
            f"  Expected: {expected_phase.name}\n"
            f"  Actual:   {actual_phase.name}"
        )

    def assert_ordered(
        self,
        before: Callable[[TraceEvent], bool],
        after: Callable[[TraceEvent], bool],
    ) -> None:
        """Assert the first event matching `before` precedes (by seq) the first
        matching `after`."""
        events = self.parse()
        first_before = next((e for e in events if before(e)), None)
        first_after = next((e for e in events if after(e)), None)
        assert first_before is not None, "No event matched `before` predicate"
        assert first_after is not None, "No event matched `after` predicate"
        assert first_before.seq < first_after.seq, (
            f"Ordering violation: {first_before.event} (seq={first_before.seq}) does "
            f"not precede {first_after.event} (seq={first_after.seq})"
        )
