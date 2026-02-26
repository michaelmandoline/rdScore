```{=html}
<p align="center">
```
`<img src="assets/rdscore-logo-color-1200.png" width="400">`{=html}
```{=html}
</p>
```
# rdScore

Lightweight PDF sheet music reader for Linux with Bluetooth pedal
support.\
Lecteur de partitions PDF pour Linux avec compatibilité pédale
Bluetooth.

Designed for musicians who need a reliable, distraction-free score
viewer for rehearsal, individual practice, recording sessions, and
concert preparation.\
Conçu pour les musiciens recherchant un lecteur de partitions fiable,
sans distraction, pour la répétition, le travail individuel,
l'enregistrement et la préparation de concert.

rdScore focuses on fullscreen readability, keyboard control, two-page
layout, and fast page navigation.\
rdScore privilégie la lisibilité plein écran, le contrôle clavier, le
mode 1 ou 2 pages et une navigation rapide entre les pages.

It works with standard Bluetooth page-turn pedals that emulate keyboard
arrow or PageUp/PageDown keys.\
Compatible avec les pédales Bluetooth standard qui simulent des touches
clavier (flèches ou PageUp/PageDown).

------------------------------------------------------------------------

## 🇬🇧 Overview

rdScore is a minimalist PDF reader designed primarily for musicians.

It provides:

-   Fullscreen concert mode\
-   Reliable keyboard navigation\
-   1-page / 2-page layout\
-   Zoom and scroll control\
-   Page extraction for individual parts\
-   Bluetooth pedal compatibility

No cloud.\
No subscription.\
No background services.\
Just a focused PDF score reader.

------------------------------------------------------------------------

## 🇫🇷 Présentation

rdScore est un lecteur PDF minimaliste conçu principalement pour les
musiciens.

Il propose :

-   Mode plein écran concert\
-   Navigation clavier fiable\
-   Mode 1 page / 2 pages\
-   Zoom et défilement\
-   Extraction de pages pour parties individuelles\
-   Compatibilité pédale Bluetooth

Pas de cloud.\
Pas d'abonnement.\
Pas de services en arrière-plan.\
Un lecteur de partitions PDF centré sur l'essentiel.

------------------------------------------------------------------------

# Features / Fonctionnalités

## Display / Affichage

-   Fullscreen mode (`f`)\
-   1-page / 2-page layout (`1` / `2`)\
-   Intelligent centering\
-   Automatic fit-to-screen\
-   Zoom (`Ctrl +`, `Ctrl -`, `Ctrl 0`)\
-   Scroll when zoomed

## Navigation

  Key                 Action
  ------------------- ------------------------------------
  ← / →               Previous / Next page (zoom = 100%)
  ↑ / ↓               Scroll when zoomed
  PageUp / PageDown   Previous / Next page
  Space / Backspace   Previous / Next page
  g                   Go to page
  e                   Extract page range
  F1 / ?              Help
  Esc                 Quit

------------------------------------------------------------------------

# Bluetooth Pedal Support

Most Bluetooth pedals emulate keyboard keys.

rdScore works out of the box with standard pedals configured as:

-   Left arrow or PageUp → Previous page\
-   Right arrow or PageDown → Next page

No specific driver or configuration required.

------------------------------------------------------------------------

# Command Line Usage / Utilisation en ligne de commande

``` bash
rdScore file.pdf
rdScore --version
```

------------------------------------------------------------------------

## Build & Installation / Compilation et installation

### English

### Dependencies

rdScore requires GTK+3, Poppler (glib binding), and qpdf.

``` bash
sudo apt install libgtk-3-dev libpoppler-glib-dev qpdf
```

### Compilation

``` bash
g++ -O2 -std=c++17 rdScore.cpp -o rdScore \
$(pkg-config --cflags --libs gtk+-3.0 poppler-glib)
```

### Local Installation

``` bash
mkdir -p ~/.local/bin
cp rdScore ~/.local/bin/
chmod +x ~/.local/bin/rdScore
```

Ensure that \~/.local/bin is present in your PATH.

### Desktop Integration

``` bash
mkdir -p ~/.local/share/applications
cp rdscore.desktop ~/.local/share/applications/
```

Optional: set as default PDF reader:

``` bash
xdg-mime default rdscore.desktop application/pdf
```

------------------------------------------------------------------------

### Français

### Dépendances

rdScore nécessite GTK+3, Poppler (binding glib) et qpdf.

``` bash
sudo apt install libgtk-3-dev libpoppler-glib-dev qpdf
```

### Compilation

``` bash
g++ -O2 -std=c++17 rdScore.cpp -o rdScore \
$(pkg-config --cflags --libs gtk+-3.0 poppler-glib)
```

### Installation locale

``` bash
mkdir -p ~/.local/bin
cp rdScore ~/.local/bin/
chmod +x ~/.local/bin/rdScore
```

Vérifiez que \~/.local/bin est présent dans votre variable
d'environnement PATH.

### Intégration au bureau

``` bash
mkdir -p ~/.local/share/applications
cp rdscore.desktop ~/.local/share/applications/
```

Optionnel : définir comme lecteur PDF par défaut :

``` bash
xdg-mime default rdscore.desktop application/pdf
```

------------------------------------------------------------------------

## License / Licence

GNU General Public License v3.0

------------------------------------------------------------------------

## Author / Auteur

Mihai Cristescu\
2026
