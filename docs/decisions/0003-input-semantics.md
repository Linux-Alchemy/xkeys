# ADR 0003 — Latched chords and shared key ownership

- **Status** Implemented for simulation, pending hands-on review
- **Date** 2026-11-08

## Context

A physical keypad control may outlive a layer change, and two controls may
share a modifier or ordinary key. Re-resolving a binding on release risks
releasing the wrong chord and leaving generated keys held. The simulator
needs to expose the same output decisions that the later device backend
will use.

## Decision

On key-down, capture the active layer and chosen chord. Hold that chord
until the control is released. Press codes in binding order and release
them in reverse order. The core counts owners per code and emits a Down
transition for the first owner and an Up transition when the last owner
releases it. A blank slot tracks the held control and emits zero transitions.

When the dedicated thumb layer control is enabled, holding the thumb
selects layer 2 and releasing it returns to layer 1. Its own slots produce
zero key transitions. Otherwise, the thumb behaves like a regular control.
A layer change affects later presses. Controls already held keep their
captured bindings even if the thumb is released first.

A wheel notch taps its selected chord using the same ownership counts.
A notch leaves no held control. Wheel click is a regular held button.
Cardinal D-pad controls behave as regular buttons. Diagonals need
hardware validation.

Duplicate presses and repeat events for held controls leave their captured
binding and ownership unchanged. The simulation emits no repeat
transitions and uses no repeat timers. The later virtual keyboard is
expected to disable autorepeat, subject to backend validation in Phase 4.

Activate a validated profile only when all controls are released, including
blank slots and the dedicated thumb. Reset releases every owned key in
reverse first-down order, clears held controls and returns to layer 1.
Reset retains the profile. The service deactivates by releasing those keys
and discarding the active resolver.

## Consequences

All outputs represent one keyboard state. A modifier held by one control
continues to affect other keys pressed on that virtual keyboard. Ownership
counts prevent early release, and the simulator's owned keys and transition
trace make overlap visible. Independent shortcuts may interact when pressed
together.

The simulated results still require hands-on review of wheel taps, the
dedicated thumb, and both layer-release orders. Device-level key delivery
and repeat policy are checked when the virtual keyboard is introduced.

## Rejected alternatives

- **Resolve on release.** A layer change between press and release could
  leave the original chord held.
- **Release each binding independently.** A shared modifier or ordinary key
  would go up while another control still owns it.
- **Forward raw repeat events.** The pad could repeatedly invoke an action
  from one held control, and a duplicate down could corrupt ownership.
