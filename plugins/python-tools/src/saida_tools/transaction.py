"""Staged project outputs with rollback on commit failure."""

from __future__ import annotations

from pathlib import Path, PurePosixPath, PureWindowsPath
import os
import shutil
import uuid


class TransactionError(RuntimeError):
    pass


def safe_relative(path: str | Path) -> Path:
    raw = str(path).replace("\\", "/")
    candidate = PurePosixPath(raw)
    windows_candidate = PureWindowsPath(str(path))
    if (
        candidate.is_absolute()
        or bool(windows_candidate.drive)
        or not candidate.parts
        or ".." in candidate.parts
    ):
        raise TransactionError(f"project path must be relative and contained: {path}")
    return Path(*candidate.parts)


def _covered(relative: Path, patterns: tuple[str, ...]) -> bool:
    value = relative.as_posix()
    pure = PurePosixPath(value)
    for pattern in patterns:
        normalized = pattern.replace("\\", "/").rstrip("/")
        if not normalized:
            continue
        if pure.match(normalized):
            return True
        if not any(character in normalized for character in "*?["):
            if value == normalized or value.startswith(normalized + "/"):
                return True
    return False


class OutputTransaction:
    def __init__(self, project_root: Path, outputs: tuple[str, ...]):
        self.project_root = project_root.resolve()
        self.outputs = outputs
        run_id = uuid.uuid4().hex
        self.root = (
            self.project_root / ".saida" / "python-tools" / "staging" / run_id)
        self.stage = self.root / "new"
        self.backup = self.root / "backup"
        self.stage.mkdir(parents=True)
        self.backup.mkdir(parents=True)
        self._closed = False

    def output_path(self, relative: str | Path) -> Path:
        safe = safe_relative(relative)
        if not _covered(safe, self.outputs):
            raise TransactionError(
                f"'{safe.as_posix()}' is not covered by declared outputs")
        path = self.stage / safe
        path.parent.mkdir(parents=True, exist_ok=True)
        return path

    def _staged_files(self) -> list[Path]:
        return sorted(
            (path for path in self.stage.rglob("*") if path.is_file()),
            key=lambda path: path.as_posix(),
        )

    def commit(self) -> list[Path]:
        if self._closed:
            raise TransactionError("transaction is already closed")
        files = self._staged_files()
        for source in files:
            relative = source.relative_to(self.stage)
            if not _covered(relative, self.outputs):
                raise TransactionError(
                    f"staged undeclared output '{relative.as_posix()}'")

        installed: list[Path] = []
        backed_up: list[tuple[Path, Path]] = []
        try:
            for source in files:
                relative = source.relative_to(self.stage)
                destination = self.project_root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                if destination.exists():
                    backup = self.backup / relative
                    backup.parent.mkdir(parents=True, exist_ok=True)
                    os.replace(destination, backup)
                    backed_up.append((destination, backup))
                os.replace(source, destination)
                installed.append(destination)
        except Exception as error:
            for destination in reversed(installed):
                if destination.exists():
                    destination.unlink()
            for destination, backup in reversed(backed_up):
                if backup.exists():
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    os.replace(backup, destination)
            raise TransactionError(f"failed to commit outputs: {error}") from error
        finally:
            self._closed = True
            shutil.rmtree(self.root, ignore_errors=True)
        return installed

    def rollback(self) -> None:
        if not self._closed:
            self._closed = True
            shutil.rmtree(self.root, ignore_errors=True)

    def __enter__(self) -> "OutputTransaction":
        return self

    def __exit__(self, _type, _value, _traceback) -> None:
        if not self._closed:
            self.rollback()
