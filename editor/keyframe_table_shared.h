#pragma once

#include <QSet>
#include <QTableWidget>

#include <functional>

namespace editor {

QSet<int64_t> collectSelectedFrameRoles(QTableWidget* table);
int64_t primarySelectedFrameRole(QTableWidget* table);
void restoreSelectionByFrameRole(QTableWidget* table, const QSet<int64_t>& frames);
QTableWidgetItem* ensureContextRowSelected(QTableWidget* table, const QPoint& pos, int* rowOut = nullptr);
int64_t rowFrameRole(QTableWidget* table, int row);
int countSelectedFrameRoles(QTableWidget* table, const std::function<bool(int64_t)>& predicate);

} // namespace editor
