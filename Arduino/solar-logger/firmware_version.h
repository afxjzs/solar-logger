// ============================================================================
// FIRMWARE IDENTITY
// ============================================================================
//
// Three different questions, three different answers. Conflating them is how a
// board ends up running something nobody can name.
//
//   Version    What humans call this firmware. Edited by hand, in this file,
//              and nowhere else. Never derived from Git, because a version is
//              a statement of intent and a commit hash is a statement of fact.
//
//   Revision   Which source produced the image. Injected at compile time by
//              tools/upload.sh from `git rev-parse --short HEAD`, with a
//              -dirty suffix when the working tree has uncommitted changes.
//              Absent from IDE and plain `arduino-cli compile` builds, and the
//              firmware says so out loud rather than printing a plausible
//              blank.
//
//   Build ID   Which generation of the command protocol this image speaks.
//              Deterministic, source-controlled, and deliberately coarse: it
//              changes when the wire protocol changes, not when a line moves.
//              It is what made a mismatched image diagnosable on 2026-09-16
//              (see DECISIONS.md D-037), so it stays.
//
// Deliberately NOT here: the compile timestamp. Two builds of the same source
// would carry different identities and two builds of different source could
// carry the same one, which is the wrong answer to both questions above.
//
// VERSION POLICY while pre-1.0 is documented in docs/PROJECT.md.

#pragma once

// ----------------------------------------------------------------------------
// HUMAN VERSION - edit this, and bump it in the same commit as the change.
// ----------------------------------------------------------------------------
//
// Pre-1.0 on purpose. 1.0.0 is reserved for firmware that has run unattended
// in the car and been read back successfully, which has not happened yet.
// 0.2.0: CMD_RESULT reports whether a command COMPLETED rather than whether
// the dispatcher recognized it (D-041). That is a change to what every
// command reports and to the host protocol, so the policy in PROJECT.md
// makes it a MINOR bump. -dev stays until an image has been confirmed on
// hardware.
// 0.3.0: RELEASE answers ERROR when its handoff fails instead of OK, and HOLD
// is refused with ERROR while a deferred autonomous sleep is pending (D-044).
// Both change what a command does or reports, so the policy makes it MINOR.
// 0.2.0 was never confirmed on hardware.
#define FIRMWARE_VERSION "0.3.0-dev"

// ----------------------------------------------------------------------------
// SOURCE REVISION - injected by the canonical build.
// ----------------------------------------------------------------------------
//
// tools/upload.sh passes:
//
//     --build-property compiler.cpp.extra_flags=-DFIRMWARE_GIT_REV="<rev>"
//
// Any other build leaves it empty, and printFirmwareIdentity() reports
// UNKNOWN. An empty default keeps every other build working unchanged; a
// fabricated default would let an unidentifiable image look identified.
#ifndef FIRMWARE_GIT_REV
#define FIRMWARE_GIT_REV ""
#endif

// ----------------------------------------------------------------------------
// PROTOCOL BUILD ID - see D-036 and D-037.
// ----------------------------------------------------------------------------
//
// Bump this when CMD_ACK / CMD_RESULT semantics, the bounded protocol writer,
// or the deferred-RELEASE handshake change. Do not bump it for ordinary work:
// its whole value is that a host can tell protocol generations apart.
// v3: CMD_RESULT semantics changed (D-041), and the deferred-sleep handshake
// that D-035 built for RELEASE now also carries LOGGER AUTONOMOUS ON, which
// previously slept without ever emitting a result. Both are exactly what this
// identifier exists to mark.
#define FIRMWARE_BUILD_ID "solar-logger-protocol-ack-v3"
