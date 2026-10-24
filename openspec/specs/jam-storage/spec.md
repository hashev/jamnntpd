# jam-storage Specification

## Purpose

Сохранять существующие JAM-базы и совместимость с внешними FTN-инструментами при безопасном конкурентном чтении и изменении сообщений.

## Requirements

### Requirement: Portable JAM format
The storage SHALL read and write the existing JAM binary format without conversion, independently of host pointer size, structure padding and endianness, retaining message numbers, CRC values, attributes, subfields and last-read records.

#### Scenario: Existing base round trip
- **WHEN** known JAM fixtures are read, appended, updated, cancelled and reopened on supported platforms
- **THEN** encoded fields and record offsets match independently prepared binary fixtures and unchanged records retain their bytes

### Requirement: Coordinated base access
The storage SHALL serialize conflicting accesses within the process and retain compatible operating-system file locks for external tossers. Failed or timed-out lock acquisition SHALL NOT modify the base.

#### Scenario: Concurrent writers
- **WHEN** sessions and a separate process contend for the same base
- **THEN** successful writes have unique consistent records and readers do not consume partially published records

#### Scenario: Lock failure
- **WHEN** another process holds the lock beyond the configured timeout
- **THEN** the operation reports failure and all locally acquired resources are released

### Requirement: Validate data and report write failures
The storage SHALL validate lengths, offsets, record boundaries and arithmetic before use, reject corrupt data and report read, write and flush failures without claiming a successful operation.

#### Scenario: Corrupt record
- **WHEN** a file is truncated or contains overflowing offsets or invalid subfield lengths
- **THEN** the request fails safely without out-of-bounds access or unbounded allocation

#### Scenario: Failed commit
- **WHEN** an injected write or flush error interrupts a mutation
- **THEN** success is not returned, rollback restores committed records when I/O permits recovery, and unpublished append data is not exposed as a valid article

#### Scenario: Persistent storage failure
- **WHEN** a persistent I/O failure prevents rollback of a partially modified record
- **THEN** the base is marked unavailable for further writes, the recovery failure is reported explicitly, and restoration from a consistent backup is required before service resumes
