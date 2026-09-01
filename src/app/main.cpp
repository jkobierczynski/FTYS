#include "gui/MainWindow.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char** argv) {
    // The .qrc holding the app icon is compiled into ls_gui, a *static*
    // library -- nothing else in it references a symbol from that specific
    // translation unit, so the linker silently drops it (and the resource
    // never registers at runtime) without this explicit call. Must be made
    // from global scope with the .qrc's base name (see Qt's docs on
    // resources inside static libraries).
    Q_INIT_RESOURCE(resources);

    QApplication app(argc, argv);
    QApplication::setApplicationName("FTYS");
    QApplication::setOrganizationName("FTYS");
    QApplication::setWindowIcon(QIcon(":/icons/ftys_logo.png"));

    ls::MainWindow window;
    window.show();

    return app.exec();
}
