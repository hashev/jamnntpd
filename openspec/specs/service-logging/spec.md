# service-logging Specification

## Purpose

Предоставлять полезную диагностику сервера при конкурентных соединениях, сохраняя настройки вывода и защищая учетные данные пользователей.

## Requirements

### Requirement: Concurrent diagnostic records
The service SHALL retain configured log paths, console suppression, debug and group-selection logging, emit complete records with timestamp, severity and session context, and flush pending diagnostics during orderly shutdown.

#### Scenario: Concurrent sessions
- **WHEN** multiple sessions log failures and group changes simultaneously
- **THEN** records are not interleaved or truncated and remain attributable to their sessions

### Requirement: Credential redaction
The service SHALL exclude passwords and AUTHINFO PASS arguments from every diagnostic sink, including protocol debug output, error paths and console output.

#### Scenario: Authentication tracing
- **WHEN** a client authenticates successfully or unsuccessfully with debug logging enabled
- **THEN** no sink contains its password or raw credential command

### Requirement: Logging failure isolation
The service SHALL report an unavailable logging destination through a bounded fallback without recursive logging, exposing credentials or terminating worker threads unexpectedly.

#### Scenario: Unwritable destination
- **WHEN** a log file cannot be opened or a sink fails during a request
- **THEN** a safe fallback diagnostic is produced and service behavior follows the documented logging failure policy
