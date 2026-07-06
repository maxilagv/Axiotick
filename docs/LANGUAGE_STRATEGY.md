# Language Strategy

Axiotick should use each language only where it improves correctness, latency, research velocity or maintainability.

## C/C++

C/C++ remains the primary language for the latency-sensitive trading core.

Use C/C++ for:

- Event bus and codecs.
- Order book and matching.
- OMS state transitions.
- Risk checks.
- Execution-critical adapters.
- Replay and benchmark paths where runtime latency matters.

Rationale:

- The current backend is already C/C++.
- Existing low-latency modules are written in C/C++.
- The team can avoid unnecessary cross-language overhead in hot paths.

Risks:

- Memory safety and concurrency bugs require disciplined tests, review and tooling.
- Manual performance optimization can reduce maintainability if not measured.

## Python

Python should be the standard language for research and AI workflows, not live hot paths.

Use Python for:

- Quantitative research.
- Historical analysis.
- Feature exploration.
- Model training and evaluation.
- NLP over news, filings or macro events.
- Notebooks with controlled inputs and reproducible outputs.
- Offline reports and model diagnostics.

Rules:

- Python must not block market data to decision to risk hot paths.
- Python models must be versioned before runtime use.
- Python outputs must be promoted through validation gates.
- Research notebooks must record dataset, feature and model versions.

Rationale:

- Python is the strongest ecosystem for data science, ML and research.
- It improves strategy iteration speed without weakening the C/C++ runtime.

## Rust

Rust should not be introduced immediately. It is a valid future option only when a component has a clear technical reason.

Consider Rust for:

- External connectors with complex concurrency.
- High-speed parsers where memory safety is valuable.
- Data ingestion services.
- Infrastructure tools with strict reliability needs.
- Components where C++ ownership/concurrency risk becomes too expensive.

Do not add Rust for:

- Branding.
- Rewriting stable C/C++ modules without evidence.
- Small helpers that add build complexity.
- Hot path components before FFI and deployment costs are understood.

Decision rule:

Rust can be adopted when the proposed component has a written design showing expected safety benefit, latency impact, build impact, operational impact and test plan.

## Current Recommendation

- Keep the low-latency core in C/C++.
- Build research and AI workflows in Python.
- Defer Rust until a concrete connector, parser or concurrent service justifies the extra language boundary.
