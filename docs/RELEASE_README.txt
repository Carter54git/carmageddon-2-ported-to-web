Carmageddon II Web - self-hosting package
=========================================
Play online without installing anything: https://dos.zone/carmageddon-2
Source code and full documentation: see the GitHub repository.

This package contains NO game files. To run it you need your own copy of
Carmageddon II: Carpocalypse Now for Windows (see GAME_FILES.md).

1. Pack your game into the site folder (Python 3):
     python3 tools/make_data.py "/path/to/Carmageddon 2" site
   Windows: drag the game folder onto tools\make_data.cmd
   Optional music: put Track02..Track08 (.ogg/.mp3/.wav) into site/music/

2. Run it:
     Windows:  server\serve.cmd                     (opens the browser)
     Node.js:  node server/relay.js --static site   -> http://localhost:8080/
     Python:   python3 server/relay.py 8080 site    -> http://localhost:8080/

3. To publish on a web site, upload the contents of site/ (see HOSTING.md);
   network play needs the relay server (see server/README.md).
