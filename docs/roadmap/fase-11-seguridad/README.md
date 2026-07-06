# Fase 11 - Seguridad

## Objetivo

Build the security baseline required before any controlled live trading.

## Estado Actual

Token auth and rate limiting exist at a basic level. Production security controls are not complete.

## Problemas Detectados

- Secrets management is not production-grade.
- RBAC and MFA are not implemented.
- Token lifecycle and audit controls need hardening.
- Live trading permissions are not separated enough.

## Subfases

- Define threat model.
- Add secret and token lifecycle management.
- Add role-based access control.
- Add live trading permission gates.

## Tareas Tecnicas

- Separate read-only, paper trading and live trading permissions.
- Audit auth, config and operator actions.
- Enforce secure defaults for production profile.
- Document key rotation and emergency revocation.

## Criterios de Aceptacion

- No production key is stored in source.
- Live execution requires explicit permission.
- Token creation, rotation and revocation are audited.

## Metricas Esperadas

- Auth failures.
- Rate-limit events.
- Token rotations.
- Privileged action audit count.

## Tests Requeridos

- Auth and authorization tests.
- Rate-limit tests.
- Permission separation tests.
- Secret-loading failure tests.

## Benchmarks Requeridos

- Auth overhead on API paths.

## Riesgos Tecnicos

- Weak key management can create catastrophic loss risk.
- Security controls can be bypassed if live and paper modes are unclear.

## Dependencias

- Configuration system.
- API contract stability.

## Resultado Esperado

A security baseline suitable for paper operations and future live readiness review.
