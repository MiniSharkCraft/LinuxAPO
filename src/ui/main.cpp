#include "MainWindow.h"

#include <QApplication>
#ifdef SKYAPO_HAVE_BRANDED_APP_ICON
#include <QIcon>
#endif

int main(int argc, char **argv) {
  QGuiApplication::setDesktopFileName("skyapo.desktop");
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("SkyAPO");
  QCoreApplication::setApplicationName("SkyAPO");
#ifdef SKYAPO_HAVE_BRANDED_APP_ICON
  app.setWindowIcon(QIcon(QStringLiteral(":/icons/skyapo.png")));
#endif
  MainWindow window(argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString{});
  window.show();
  return app.exec();
}
