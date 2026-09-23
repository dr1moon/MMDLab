# MMDLab Agent Instructions

`CLAUDE.md` is the canonical project architecture and engineering guide. Read and follow it before changing source code, build configuration, project documentation, or generated-project workflows.

## Commit Rules

- Write commit subjects and bodies in English.
- Use an imperative, present-tense subject line no longer than 72 characters.
- Keep each commit focused on one coherent, buildable change.

## Build Rules

- Edit `premake5.lua`; never hand-edit generated Visual Studio solution or project files.
- Regenerate project files through `GenerateProjects.bat` after changing build targets or source-file lists.
- Run `GenerateProjects.bat --build Debug` and `Build\Bin\Debug\x64\MmdTests.exe` when validating build or test changes.
