# Building rdScore

rdScore is developed and tested on Linux.

The program depends on GTK and Poppler for PDF rendering.
Page extraction relies on libqpdf.

Requirements
GTK+ 3
poppler-glib
libqpdf

---

## Tested on

rdScore has been tested on:

- Linux Mint Cinnamon
- Linux Mint XFCE

Other GTK-based Linux environments should work as well.

---

sudo apt install \
    build-essential \
    libgtk-3-dev \
    libpoppler-glib-dev \
    libqpdf-dev

Note: PDF extraction is now handled internally using libqpdf.

---

## Compilation

Compile the program with:

g++ -O2 -std=c++17 rdScore.cpp -o rdScore $(pkg-config --cflags --libs gtk+-3.0 poppler-glib libqpdf)

---

## Installation (local user)

Copy the binary to a directory in your PATH:

cp rdScore ~/.local/bin/

Make sure it is executable:

chmod +x ~/.local/bin/rdScore

---

## Run

Run the program with:

rdScore

or open a PDF directly:

rdScore file.pdf
