# Changelog

rdScore 1.1.2
--------------

Stability and ergonomics improvements.

Navigation
- Page navigation now advances one page at a time in both 1-page and 2-page display modes.
- In 2-page mode, navigation stops correctly at the last page pair (no longer displays the last page alone).

Setlists
- Added Manage Setlists dialog for centralized management.
- Improved navigation flow between Manage Setlists, Setlist view and PDF view.
- Remember selected setlist entry when returning from a score.

File handling
- File → Open now reopens the last directory used.
- Improved error reporting when extraction fails.

Extraction
- Fixed incorrect error message when qpdf was present but extraction failed for another reason.

General
- Multiple usability and stability improvements.
---

rdScore 1.1.1 – 2026-03-08

Improvements
- Menu accessible from keyboard
- Permanent display of page and zoom in menu bar
- Setlist window usability improvements

New features
- Rename setlist
- Add multiple PDF files to setlists (Create/Edit)

Bug fixes
- GTK warnings on exit fixed
- Improved stability of print dialog
---

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
