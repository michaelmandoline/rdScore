# Building rdScore

rdScore is developed and tested on Linux.

The program depends on GTK and Poppler for PDF rendering.
Page extraction relies on qpdf.

Requirements
- GTK+ 3
- Poppler (poppler-glib)
- qpdf

---

## Tested on

rdScore has been tested on:

- Linux Mint Cinnamon
- Linux Mint XFCE

Other GTK-based Linux environments should work as well.

---

## Requirements

Install the required packages:

Ubuntu / Debian / Linux Mint:

sudo apt install \
    build-essential \
    libgtk-3-dev \
    libpoppler-glib-dev \
    qpdf

---

## Compilation

Compile the program with:

g++ -O2 -std=c++17 rdScore.cpp -o rdScore \
$(pkg-config --cflags --libs gtk+-3.0 poppler-glib)

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
