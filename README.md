# AmigaMac

AmigaOS réécrit de zéro en application macOS native. Pas d'émulateur :
l'OS (exec, dos, Shell…) est du code natif ARM/Intel. Seul le **processeur 68k**
est interprété, pour exécuter les vrais programmes Amiga.

![GfxDemo](docs/m3-menu.png)

## Architecture

```
 programme Amiga 68k ──JSR -n(A6)──▶ table de vecteurs : $Axxx + RTS
                                                │ (piège A-line)
                                                ▼
 interpréteur 68020 (cpu68k.c) ──▶ fonction native (exec.c, dos.c…)
                                                │
 mémoire 68k (32 Mo, big-endian) ◀──────────────┘  structures Amiga réelles
                                                   (ExecBase, Process, FileHandle…)
 macOS : fichiers (SYS:, RAM:, Mac:), fenêtre AmigaShell (AppKit)
```

- `Sources/CAmiga/cpu68k.c` : interpréteur 68000 + 68020 (sans FPU pour l'instant).
- `Sources/CAmiga/core.c` : mémoire, tas, pièges, listes exec, création des bibliothèques.
- `Sources/CAmiga/fdtables.c` : **généré** depuis les `.fd` du NDK (`tools/genfd.py`).
  Les 76 bibliothèques ont leurs 1 292 vecteurs aux bons offsets. Une fonction non
  encore écrite affiche son nom (`fonction non implémentée : dos.Foo`).
- `Sources/CAmiga/sched.c` : l'ordonnanceur d'exec (voir plus bas).
- `Sources/CAmiga/exec.c`, `dos.c`, `timer.c` : les bibliothèques et périphériques natifs.
- `Sources/CAmiga/shell.c` : le Shell et ses commandes intégrées.
- `Sources/AmigaMac/main.swift` : l'application macOS.
- `Sources/amigarun` : la même chose en ligne de commande (tests, scripts).

Méthode : réécriture propre, d'après l'ABI publique (NDK, Autodocs). Le code de
la ROM Kickstart n'est jamais recopié.

## Multitâche

Chaque tâche Amiga a son thread macOS et son propre contexte 68k, mais un seul
s'exécute à la fois : il détient le « bâton » (verrou global). L'exécution reste
donc déterministe, sans accès concurrent à la mémoire Amiga. Une fonction native
qui doit attendre (`Wait`, `WaitPort`, `DoIO`, lecture console, `Delay`) passe
simplement le bâton.

- État visible identique à exec : `ThisTask`, listes `TaskReady`/`TaskWait`,
  `tc_State`, signaux, `TDNestCnt`/`IDNestCnt` sauvegardés par tâche.
- Préemption toutes les 20 000 instructions (tourniquet à priorité égale), et
  immédiatement quand un `Signal`/`PutMsg` réveille une tâche plus prioritaire.
  `Forbid`/`Disable` la suspendent ; `Wait` les rompt temporairement, comme sur Amiga.
- Événements extérieurs (échéances du timer.device, lignes tapées, Ctrl-C) : une
  file que l'interface remplit sans jamais prendre le bâton. Un programme qui
  boucle sans fin ne peut pas geler la fenêtre.
- Le Shell est un vrai processus CLI (natif). `RUN` lance un processus CLI en
  arrière-plan, `STATUS` les liste, `BREAK n` (ou Ctrl-C) les interrompt.

Implémentés : `AddTask`/`RemTask`, `Wait`/`Signal`/`SetSignal`, `AllocSignal`,
`SetTaskPri`, ports et messages, sémaphores avec file d'attente, `OpenDevice`,
`DoIO`/`SendIO`/`WaitIO`/`CheckIO`/`AbortIO`, `CreateIORequest`, `timer.device`
(toutes unités, `TR_ADDREQUEST`/`GETSYSTIME`/`SETSYSTIME`, `AddTime`, `SubTime`,
`CmpTime`, `GetSysTime`, `ReadEClock`), `CreateNewProc`/`CreateProc`, `Delay`,
`WaitForChar`.

## Graphisme et Intuition

- **Un écran Amiga = une fenêtre macOS.** Le Workbench (640×256, 8 couleurs) s'ouvre
  à la demande ; les écrans personnalisés (`OpenScreen`) ouvrent leur propre fenêtre.
  Affichage environ 50 fois par seconde, et seulement quand quelque chose a changé ;
  pixels nets (plus proche voisin), rapport d'aspect respecté.
- **Chaque fenêtre a sa bitmap**, composée à l'affichage dans l'ordre de profondeur :
  aucune fenêtre n'est jamais abîmée (comportement `SMART_REFRESH` parfait), donc
  `layers.library` reste minimale.
- **graphics.library** travaille sur de vraies bitmaps planaires en mémoire 68k :
  pixels, lignes à motif, rectangles à motif, `Area*` (polygones et ellipses remplis),
  ellipses, `BltBitMap` (tous minterms), `ClipBlit`, `BltTemplate`, `ScrollRaster`,
  `Flood`, texte générique (`tf_CharData`/`tf_CharLoc`), couleurs 4 et 32 bits,
  stylos, pixels chunky, `WaitTOF`, régions.
- **Police système** : `fonts/amiga8.txt`, une police 8×8 dessinée pour AmigaMac
  dans l'esprit de topaz (la topaz de la ROM est sous copyright), accents Latin-1
  composés automatiquement (`tools/genfont.py`).
- **Intuition** : écrans, fenêtres et décorations Workbench 2/3 (fermeture, profondeur,
  taille, barre de déplacement), activation, IDCMP (`CLOSEWINDOW`, `MOUSEBUTTONS`,
  `MOUSEMOVE`, `RAWKEY`, `VANILLAKEY`, `GADGETUP/DOWN`, `MENUPICK`, `NEWSIZE`,
  `CHANGEWINDOW`, `ACTIVE/INACTIVEWINDOW`, `INTUITICKS`), gadgets booléens, chaîne
  (édition au clavier) et proportionnels, menus (sous-menus, `CHECKIT`, exclusion
  mutuelle, raccourcis Amiga), `EasyRequest`/`AutoRequest`/`BuildEasyRequest`,
  `DrawBorder`/`DrawImage`/`PrintIText`. La tâche `input.device` (priorité 20)
  traite souris et clavier comme sur Amiga.
- Les décalages de structures viennent de `include/ndk_offsets.h`, **généré par le
  compilateur Amiga (vbcc) exécuté dans AmigaMac** à partir des en-têtes officiels
  (`Tests/c/offsets.c`).

Pas encore fait : `GIMMEZEROZERO`, couches créées par les programmes, polices sur disque.

## Workbench et bibliothèques d'interface

- **Le Workbench** démarre au lancement (`S:Startup-Sequence` contient `LoadWB`).
  C'est un processus natif écrit pour AmigaMac, qui passe par les vraies fonctions
  d'intuition, gadtools et asl : bureau avec les volumes (System, Ram Disk, Mac),
  tiroirs avec ascenseur, affichage par icônes ou par nom, « Icônes seulement » /
  « Tous les fichiers », sélection par clic, Maj-clic et lasso, glisser-déposer
  (réorganiser, déplacer dans un tiroir, déposer sur un outil ou une AppWindow),
  double-clic. Menus Workbench / Fenêtre / Icônes / Outils : exécuter une commande,
  nouveau tiroir, parent, ranger, copier, renommer, informations, figer, supprimer,
  lancer un programme (sélecteur ASL), etc. Les programmes sont lancés comme sur Amiga :
  processus sans CLI qui reçoit un message `WBStartup`, rendu à la fin et nettoyé.
  Un fichier du Mac sans outil Amiga s'ouvre avec son application macOS.
- **workbench.library** : AppWindow, AppIcon, AppMenuItem (messages `AppMessage`),
  `OpenWorkbenchObject`, `UpdateWorkbench`, `WBInfo`.
- **icon.library** : lecture et écriture du format `.info` (DiskObject, images
  planaires, outil par défaut, ToolTypes, données de tiroir), icônes par défaut
  dessinées pour AmigaMac (`fonts/icons.txt` → `tools/genicons.py`), `FindToolType`,
  `MatchToolValue`, `BumpRevision`, `DrawIconState`, `GetIconTagList`…
- **BOOPSI** : vraies structures `IClass`/`_Object`, sous-classes 68k (dispatchers en
  C Amiga avec `DoSuperMethodA`), classes système `rootclass`, `imageclass`,
  `frameiclass`, `sysiclass`, `itexticlass`, `fillrectclass`, `gadgetclass`,
  `buttongclass`, `frbuttonclass`, `propgclass`, `strgclass`, `groupgclass`,
  `icclass`, `modelclass`, notifications `ICA_TARGET` (jusqu'à `IDCMPUPDATE`).
- **gadtools.library** : tous les types (bouton, case, entier, liste, MX, nombre,
  cycle, palette, ascenseur, curseur, chaîne, texte, générique) avec leurs messages
  IDCMP, `GT_Set/GetGadgetAttrs`, `CreateMenus`/`LayoutMenus` (sous-menus, barres,
  `GTMENUITEM_USERDATA`), `DrawBevelBox`, `GetVisualInfo`.
- **asl.library** : sélecteur de fichiers (motif, tiroir, fichier, volumes, parent,
  double-clic, filtres `AcceptPattern`/`RejectIcons`…), de police et de mode d'écran.
- **utility.library** (étiquettes, hooks, dates, arithmétique 32/64 bits) et motifs
  AmigaDOS (`#?`, `(a|b)`, `~`, `[a-z]`) dans `ParsePattern`/`MatchPattern`.

## Construire et lancer

```bash
tools/makeapp.sh && open build/AmigaMac.app
```

Le dossier système est créé au premier lancement dans
`~/Library/Application Support/AmigaMac/System` (`AMIGAMAC_SYS` pour en choisir un autre).
`SYS:` = ce dossier, `RAM:` = un sous-dossier temporaire, `Mac:` = tout le disque du Mac.

En ligne de commande :

```bash
swift build && .build/debug/amigarun System "Hello Claude LOUD"
```

## Tests

- `tools/cpucheck.sh 1000000` : compare l'interpréteur à Musashi (cœur 68k sous licence MIT,
  téléchargé dans `tools/oracle`, outil de test uniquement), une instruction aléatoire à la
  fois : registres, drapeaux, mémoire, cadres d'exception. Résultat : 0 différence sur
  4 × 1 000 000 d'instructions. Sont exclus les cas où Musashi s'écarte du manuel Motorola
  et de WinUAE (`PACK`/`UNPK` en mémoire, `CHK2`/`CMP2`), et le bit M (pile maître,
  inutilisé sur Amiga).
- `Tests/asm/hello.s` : vrai exécutable Amiga (hunk). Il utilise `Write`, `PutStr`,
  `VPrintf`/`RawDoFmt` et `ReadArgs`.
- `Tests/asm/tasks.s` (`TaskTest`) : messages entre tâches, préemption de deux
  tâches qui ne cèdent jamais, attente timer.device de 300 ms mesurée,
  `CreateNewProc`, sémaphore disputé. 20 exécutions sur 20 réussies.
- `Tests/asm/spin.s` (`Spin`) : un point toutes les 0,1 s, s'arrête sur Ctrl-C.
  Sert à tester `RUN`, `STATUS` et `BREAK`.
- Programmes C compilés pour Amiga avec vbcc (`tools/vcc`, outil de développement) :
  `Tests/c/gfxdemo.c` (fenêtre, dessin, bouton, champ texte, menus, requêtes),
  `screendemo.c` (écran 32 couleurs), `idcmplog.c` (affiche chaque message IDCMP),
  `hello.c`, `offsets.c`, `boopsitest.c` (sous-classe 68k de `buttongclass`, notifications),
  `gtdemo.c` (tous les gadgets et menus GadTools), `icontest.c` (aller-retour `.info`,
  ToolTypes, icônes par défaut), `asltest.c` (sélecteurs fichier, police, mode d'écran),
  `wbapp.c` (AppWindow, AppIcon, AppMenuItem, `OpenWorkbenchObject`).
- Le Workbench se teste aussi sans interface :
  `amigarun System "loadwb" "wait 2" "input click 62 44" "input click 62 44" "snapshot RAM:wb.ppm"`.
- Commandes de test du Shell : `INPUT` injecte souris/clavier, `SNAPSHOT` écrit
  l'image de l'écran actif (PPM), `WAIT`. Les interactions se scénarisent sans interface :
  `amigarun System "run GfxDemo" "wait 1" "input click 300 140" "snapshot RAM:a.ppm"`.

## Feuille de route

| Jalon | Contenu | État |
|---|---|---|
| 1. Socle | CPU 68k, mémoire, pièges, exec/dos de base, LoadSeg, Shell, app macOS | fait |
| 2. Multitâche | ordonnanceur exec (tâches, signaux, Wait, ports, sémaphores, I/O), CreateNewProc, RUN, timer.device | fait |
| 3. Graphisme | graphics, layers, intuition : écrans et fenêtres dans macOS, IDCMP, gadgets, menus, requêtes | fait |
| 4. Workbench | icon.library, Workbench (icônes, tiroirs, menus), BOOPSI, gadtools, asl | fait |
| 5. Compatibilité | FPU 68881/2, utility/locale/diskfont, tests sur de vrais logiciels Aminet | à faire |
