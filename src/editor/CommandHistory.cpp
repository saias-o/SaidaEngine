#include "editor/CommandHistory.hpp"
#include "editor/SceneDocument.hpp"

#include <algorithm>

namespace saida {

void CommandHistory::execute(std::unique_ptr<Command> command) {
    if (!command) return;
    command->execute(document_);
    undoStack_.push_back(std::move(command));
    redoStack_.clear();
    trimToBudget();
}

void CommandHistory::undo() {
    if (undoStack_.empty()) return;
    auto command = std::move(undoStack_.back());
    undoStack_.pop_back();
    command->undo(document_);
    redoStack_.push_back(std::move(command));
}

void CommandHistory::redo() {
    if (redoStack_.empty()) return;
    auto command = std::move(redoStack_.back());
    redoStack_.pop_back();
    command->execute(document_);
    undoStack_.push_back(std::move(command));
}

size_t CommandHistory::totalBytes() const {
    size_t bytes = 0;
    for (const auto& command : undoStack_) bytes += command->memoryCost();
    for (const auto& command : redoStack_) bytes += command->memoryCost();
    return bytes;
}

void CommandHistory::trimToBudget() {
    while (!undoStack_.empty() &&
           (undoStack_.size() + redoStack_.size() > kMaxCommands || totalBytes() > kMaxBytes))
        undoStack_.erase(undoStack_.begin());
}

void CommandHistory::clear() {
    undoStack_.clear();
    redoStack_.clear();
}

} // namespace saida
