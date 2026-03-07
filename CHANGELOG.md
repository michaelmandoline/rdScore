# Changelog

## Version 1.1.0 - 2026-03

### Added

- Main menu when launching rdScore without arguments
- Setlists support
- Dedicated folder for setlists
- Sidebar visible in setlist mode
- Ability to add/remove/reorder files inside a setlist
- Setlist deletion with preview and confirmation
- Simple printing support

### Improved

- Extraction dialog accepting:
  - single page (example: 7)
  - page ranges (example: 10-14)

### Behaviour changes

- Launching rdScore without arguments opens the main menu
  instead of returning an error (as in version 1.0.x)

### Design rules enforced

- Single-window application
- Esc key always returns to the previous level
- No hidden state
- No automatic page memory
- No print preview

---

## 1.0.6 - 2026-03

### Fixed

- Numeric keypad keys (KP_1, KP_2) now correctly handled
  for page layout shortcuts (1-page / 2-page).

---

## 1.0.5 - 2026

- Removed small usability frictions without architectural changes.
- Added numeric keypad support (KP_1 / KP_2).
- Improved cursor handling in fullscreen dialogs.
- Clarified navigation behavior (arrow keys pan when zoomed).
- Improved reliability of page overlay display.
- Minor keyboard documentation corrections.

---

## 1.0.4 - 2026

### Fixed

- Fullscreen centering correction
- Page number overlay behavior
- Page extraction: fixed single-page extraction (start == end)
- Desktop filename correction
- Versioning aligned (internal = release version)

---

## 1.0.0 - 2026

Initial public release.

### Features

- Fullscreen mode
- 1-page / 2-page layout
- Zoom and scroll
- Keyboard navigation
- Bluetooth pedal support
- Page extraction (qpdf)
