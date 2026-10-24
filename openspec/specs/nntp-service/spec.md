# nntp-service Specification

## Purpose

Обеспечить совместимый доступ NNTP к эхопочте и нетмейлу с безопасной авторизацией, надежным транспортом и предсказуемым завершением сервера.

## Requirements

### Requirement: Existing client contracts
The service SHALL preserve supported CLI options, configuration syntax, NNTP commands, article identifiers and documented FTN behavior, except documented defect corrections and removal of OS/2 builds.

#### Scenario: Existing deployment
- **WHEN** existing valid configuration is used on Linux, Windows or macOS
- **THEN** groups, user options, message identifiers, reading, posting, replies and cancellation retain their documented meaning

#### Scenario: Command compatibility
- **WHEN** a client uses ARTICLE, HEAD, BODY, STAT, GROUP, LIST, NEXT, LAST, XOVER, AUTHINFO, POST, MODE, HELP, QUIT or the existing stub commands
- **THEN** response codes and framing follow the documented compatibility fixtures, including empty/rejected stub responses

### Requirement: Authorization and netmail privacy
The service SHALL enforce allow rules, authenticated group permissions, netmail visibility and cancellation ownership on every applicable command; diagnostics SHALL NOT expose credentials.

#### Scenario: Unauthorized access
- **WHEN** a guest or authenticated user requests an unauthorized group, private article, overview, post or cancellation
- **THEN** access is denied without exposing message contents or changing storage

### Requirement: Reliable bounded input and output
The service SHALL handle partial network writes, interrupted operations, fragmented input, disconnects and timeouts without truncated successful responses or treating an overlong line as multiple commands. Input and article sizes SHALL have documented finite limits.

#### Scenario: Fragmentation and backpressure
- **WHEN** commands arrive byte by byte and writes complete only partially
- **THEN** complete responses retain CRLF framing and dot stuffing

#### Scenario: Malformed and excessive input
- **WHEN** input exceeds a configured limit or a client disconnects during POST
- **THEN** the service rejects or closes the session predictably without publishing an incomplete article

### Requirement: Coordinated service lifetime
The service SHALL enforce the configured session limit atomically, stop accepting sessions on termination, interrupt idle session waits and wait for all workers before releasing shared resources.

#### Scenario: Shutdown under load
- **WHEN** termination occurs during concurrent reads, posts, idle sessions or connection startup
- **THEN** all workers finish within the documented shutdown deadline, locks and sockets are released and successful articles remain readable

#### Scenario: Worker startup failure
- **WHEN** worker creation fails or simultaneous connections exceed the configured limit
- **THEN** rejected sockets and session reservations are released without leaking resources
