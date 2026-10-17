# Studio maintenance mode

The web studio under `studio/` is in maintenance mode. It stays working as a reference for the native engine's panels, and the native engine is where new work goes.

## Accepted changes

- Security fixes, including XSS-class fixes such as the HTML-sink fix.
- Build or test breaks caused by a dependency or the SDK pin.
- Bug fixes needed to keep the studio usable as a reference for the native panels.

## Not accepted

- Refactors.
- New panels or features.
- Dependency upgrades beyond security fixes.
- Design-system work.
- Performance work without a measurement.

## Where new ideas go

- The native engine, or an issue describing the behaviour the native panel should have.
- Not new TypeScript in `studio/`.
