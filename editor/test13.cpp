#include <memory>
#include "qt_compat.h"
#include <QExplicitlySharedDataPointer>
#include <QImage>
class MyClass {};
Q_DECLARE_METATYPE(MyClass)
#include <QtCore/QMutex>
int main() { return 0; }
