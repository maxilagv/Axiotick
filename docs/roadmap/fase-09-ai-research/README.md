# Fase 09 - AI Research

## Objetivo

Establish Python-based research, model training and analysis workflows without placing Python in live hot paths.

## Estado Actual

Python integration is conceptual/placeholder-level. The production model pipeline does not exist.

## Problemas Detectados

- No feature store.
- No model registry.
- No model validation, promotion or rollback process.
- No explainability standard for AI-assisted signals.

## Subfases

- Define Python research environment.
- Add dataset and feature versioning.
- Add model registry design.
- Add model evaluation and promotion gates.

## Tareas Tecnicas

- Keep notebooks controlled and reproducible.
- Store model metadata, training data references and metrics.
- Produce explanation payloads for model-driven signals.
- Define drift detection and model quarantine rules.

## Criterios de Aceptacion

- No model can be promoted without validation evidence.
- Runtime strategies reference explicit model versions.
- Python code does not block live market data to risk hot paths.

## Metricas Esperadas

- Model evaluation metrics by strategy type.
- Drift metrics.
- Feature freshness.
- Training and inference latency by mode.

## Tests Requeridos

- Feature calculation tests.
- Model metadata validation tests.
- Promotion gate tests.
- Reproducibility tests for sample research runs.

## Benchmarks Requeridos

- Offline feature generation throughput.
- Online feature lookup latency.
- Model inference latency outside hot execution paths.

## Riesgos Tecnicos

- AI claims can become misleading without governance.
- Research notebooks can become unreproducible.

## Dependencias

- Historical data and feature store design.
- Signal engine contracts.

## Resultado Esperado

A governed research layer that supports quant work without weakening runtime reliability.
