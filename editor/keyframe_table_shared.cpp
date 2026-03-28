#include "keyframe_table_shared.h"

namespace editor {

QSet<int64_t> collectSelectedFrameRoles(QTableWidget* table) {
    QSet<int64_t> frames;
    if (!table) {
        return frames;
    }
    const QList<QTableWidgetItem*> selectedItems = table->selectedItems();
    for (QTableWidgetItem* item : selectedItems) {
        if (!item) {
            continue;
        }
        const QVariant frameData = item->data(Qt::UserRole);
        if (!frameData.isValid()) {
            continue;
        }
        frames.insert(frameData.toLongLong());
    }
    return frames;
}

int64_t primarySelectedFrameRole(QTableWidget* table) {
    const QSet<int64_t> frames = collectSelectedFrameRoles(table);
    if (frames.isEmpty()) {
        return -1;
    }
    int64_t primaryFrame = -1;
    for (int64_t frame : frames) {
        if (primaryFrame < 0 || frame < primaryFrame) {
            primaryFrame = frame;
        }
    }
    return primaryFrame;
}

void restoreSelectionByFrameRole(QTableWidget* table, const QSet<int64_t>& frames) {
    if (!table || frames.isEmpty()) {
        return;
    }
    for (int row = 0; row < table->rowCount(); ++row) {
        QTableWidgetItem* item = table->item(row, 0);
        if (!item) {
            continue;
        }
        const QVariant frameData = item->data(Qt::UserRole);
        if (!frameData.isValid()) {
            continue;
        }
        if (frames.contains(frameData.toLongLong())) {
            table->selectRow(row);
        }
    }
}

QTableWidgetItem* ensureContextRowSelected(QTableWidget* table, const QPoint& pos, int* rowOut) {
    if (rowOut) {
        *rowOut = -1;
    }
    if (!table) {
        return nullptr;
    }
    QTableWidgetItem* item = table->itemAt(pos);
    if (!item) {
        return nullptr;
    }
    const int row = item->row();
    if (rowOut) {
        *rowOut = row;
    }
    if (!table->selectionModel()->isRowSelected(row, QModelIndex())) {
        table->clearSelection();
        table->selectRow(row);
    }
    return item;
}

int64_t rowFrameRole(QTableWidget* table, int row) {
    if (!table || row < 0 || row >= table->rowCount()) {
        return -1;
    }
    QTableWidgetItem* item = table->item(row, 0);
    if (!item) {
        return -1;
    }
    const QVariant frameData = item->data(Qt::UserRole);
    return frameData.isValid() ? frameData.toLongLong() : -1;
}

int countSelectedFrameRoles(QTableWidget* table, const std::function<bool(int64_t)>& predicate) {
    if (!table) {
        return 0;
    }
    int count = 0;
    const QList<QTableWidgetSelectionRange> ranges = table->selectedRanges();
    for (const QTableWidgetSelectionRange& range : ranges) {
        for (int row = range.topRow(); row <= range.bottomRow(); ++row) {
            const int64_t frame = rowFrameRole(table, row);
            if (frame >= 0 && (!predicate || predicate(frame))) {
                ++count;
            }
        }
    }
    return count;
}

} // namespace editor
