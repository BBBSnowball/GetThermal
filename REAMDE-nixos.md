Nix shell for development:

    nix-shell -p cmake qt5.qmake libjpeg libusb1 pkg-config qt5.qtquickcontrols2 qt5.qtmultimedia qt5.qtbase libsForQt5.wrapQtAppsHook gdb
    # or: nix develop github:BBBSnowball/nixcfg#GetThermal

    # create Makefile (must be in a subdir)
    # (only required once if you don't change the project)
    mkdir build && cd build && qmake ../GetThermal.pro

Build and run (in the Nix shell, see above):

    cd build  # if you aren't already in there
    rm -rf /home/user/.cache/.GetThermal-wrapped/qmlcache  # clean qml cache, otherwise old qml will be used
    rm release/.GetThermal-wrapped* release/GetThermal -f && make -j && wrapQtApp release/GetThermal && ./release/GetThermal

For debugging, apply the environment changes in ./release/GetThermal to the shell session:

    . <(grep -v '^exec ' release/GetThermal)
    gdb --args release/.GetThermal-wrapped
