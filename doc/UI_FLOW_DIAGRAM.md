# rdScore – UI Flow Diagram

Application navigation model.

---

Start

        │
        ▼

   Main Menu
   ├── Open file
   ├── Open setlist
   └── Quit

        │
        ▼

   Setlist list
        │
        ▼

   Setlist view (sidebar visible)
        │
        ▼

   PDF viewer

---

Esc navigation hierarchy

Dialog
   ↓ Esc
PDF opened from setlist
   ↓ Esc
Setlist view
   ↓ Esc
Setlist list
   ↓ Esc
Main menu
