#pragma once

#include "editor/Command.hpp"

#include <memory>
#include <vector>

namespace saida {

class SceneDocument;

// Undo/redo stack. execute() runs a command and records it; undo()/redo() walk
// the history. Pushing a new command clears the redo stack (standard linear
// history).
class CommandHistory {
public:
    explicit CommandHistory(SceneDocument& document) : document_(document) {}
    void execute(std::unique_ptr<Command> command);

    bool canUndo() const { return !undoStack_.empty(); }
    bool canRedo() const { return !redoStack_.empty(); }
    void undo();
    void redo();
    void clear();

    // Names for menu labels ("Undo Rename", etc.); empty if nothing to do.
    const char* undoName() const { return canUndo() ? undoStack_.back()->name() : ""; }
    const char* redoName() const { return canRedo() ? redoStack_.back()->name() : ""; }
    size_t undoCount() const { return undoStack_.size(); }
    size_t redoCount() const { return redoStack_.size(); }

private:
    void trimToBudget();
    size_t totalBytes() const;

    static constexpr size_t kMaxCommands = 256;
    static constexpr size_t kMaxBytes = 64u * 1024u * 1024u;
    SceneDocument& document_;
    std::vector<std::unique_ptr<Command>> undoStack_;
    std::vector<std::unique_ptr<Command>> redoStack_;
};

} // namespace saida
