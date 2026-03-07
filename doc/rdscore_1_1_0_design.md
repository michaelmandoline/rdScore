rdScore 1.1.0
Design and Functional Roadmap

1. Core Philosophy
rdScore remains a minimalist, single-window application designed for focused work, especially for musicians practicing at home.
The program follows a strict design philosophy:
    • Single window only
    • One task at a time (musician mindset)
    • No hidden state
    • No automatic history or last-page memory
    • Predictable navigation
The application must never surprise the user.

Esc behaviour
The Esc key always means:
Go back one level.
There must never be modal traps or unexpected behaviour.
Dialogs must always close first using:
    • Esc
    • or their Exit / Close button
before any higher-level action occurs.

2. Esc Hierarchy Model
Navigation follows a strict hierarchy.
Dialog
   ↓ Esc
PDF opened from setlist
   ↓ Esc
Setlist list
   ↓ Esc
Main menu
Rules:
    • Esc closes dialogs first
    • If a PDF was opened from a setlist, Esc returns to the setlist list
    • Esc from the setlist list returns to the main menu
This behaviour must remain consistent everywhere in the application.

3. Version 1.1.0 — Key Principles
Version 1.1.0 introduces Setlists while keeping the application simple.

Setlists folder
Setlists are stored in a dedicated folder.
Example:
~/rdscore/setlists/

Program start behaviour
When rdScore is launched without arguments, it opens the main menu.
This replaces the error behaviour present in version 1.0.x.
Main menu options:
Open file
Open setlist
Quit

Setlist behaviour
A setlist contains an ordered list of PDF files.
However, navigation is not forced to follow the order.
Users may open items freely, for example:
7
10
3
The list simply defines an ordered collection, not a strict playback sequence.

Setlist editing
Users can modify setlists:
    • add files
    • remove files
    • reorder files

Setlist deletion
Deleting a setlist requires two steps:
    1. preview the content of the setlist
    2. confirm deletion
This avoids accidental loss of information.

Handling missing files
If a file referenced in a setlist has been:
    • moved
    • renamed
    • deleted
rdScore automatically removes the invalid entry from the setlist.

4. UI Model
The interface must remain simple and predictable.

Single window
rdScore is a single-window application.
No additional permanent windows or complex layouts.

Sidebar behaviour
The sidebar appears only in setlist mode.
Characteristics:
    • visible while browsing a setlist
    • toggleable (for example using Tab)
    • disappears automatically when opening a PDF

Navigation
Navigation strictly follows the Esc hierarchy model.

5. Menu Structure
The menu remains simple.
File
   Open
Print
Setlists
   New
   List setlists
   Add files
   Remove files
   Delete setlist (after preview and confirmation)
Help
About
Quit

6. Print
Printing remains simple and direct.
Features:
    • choose a printer or a PDF output file
    • print the entire document
    • print a single page
    • print a page range
There is no print preview.
The goal is to keep the operation fast and straightforward.

7. Launch Behaviour
If rdScore is started without specifying a file, the program opens the main menu instead of producing an error.
This corrects the behaviour of version 1.0.x.

8. Extraction Module
The extraction module should use a single dialog window.
Accepted input formats:
7
Extract page 7.
10-14
Extract pages 10 to 14.
Validation rule:
p1 ≤ p2
If the rule is violated, the input must be rejected.

9. Design Summary
rdScore 1.1.0 follows strict design principles:
    • minimalist interface
    • single-window architecture
    • predictable navigation
    • Esc-based hierarchy
    • no hidden behaviour
    • no unnecessary complexity
The goal is to keep rdScore simple, fast, and reliable.


