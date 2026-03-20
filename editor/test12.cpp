#include <memory>
#include "qt_compat.h"
#include <QExplicitlySharedDataPointer>
#include <QImage>
namespace std {
template<>
struct hash<int> {
    size_t operator()(int) const { return 0; }
};
}
#include <QtCore/QMutex>
int main() { return 0; }
