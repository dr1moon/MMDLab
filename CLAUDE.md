# MMDLab

MMDLab is a personal experimental project that incrementally builds a minimal MikuMikuDance (MMD) runtime: PMX/VMD parsing, animation evaluation, and bare DirectX 12 rendering. PMX and VMD are canonical file-format identifiers. The project is also a constrained experiment in data-oriented runtime design; general scheduling abstractions must emerge from measured, real dependency branches.

## Language

- Use English only for directory names, file names, source code, comments, commit messages, build output, and project documentation.
- Keep identifiers descriptive and consistent with existing naming.
- In prose documentation, spell out an abbreviation at its first use and place the abbreviation in parentheses, for example `single-producer, single-consumer (SPSC)`. Do not use an unexplained abbreviation.
- A canonical file-format identifier with no reliable expansion, such as `PMX` or `VMD`, must be introduced as a file-format identifier rather than given an invented expansion.

## Repository Layout

- `Source/`: C++, HLSL, and all project source files.
- `Tools/premake5.exe`: the project-file generator.
- `Tools/Generate-CpuTopology.ps1`: local Windows topology probe that writes an Obsidian Mermaid note.
- `Tools/CPU-Topology-Note-Format.md`: output contract for the CPU topology probe.
- `premake5.lua`: the single build configuration entry point.

## Engineering Rules

- Use modern C++ with bare Win32, DXGI, and Direct3D 12. Do not introduce a game engine or rendering abstraction framework.
- Keep central processing unit (CPU) asset parsing, animation evaluation, and graphics processing unit (GPU) rendering separated by explicit interfaces.
- Make resource ownership, error handling, GPU synchronization, and resource states explicit. Prefer resource acquisition is initialization (RAII) and avoid hidden global state.
- Treat the Runtime Data Graph as the architectural model and typed queues as transport mechanisms. Do not add a generic data bus, event bus, or scheduler before a real dependency branch requires it.
- Keep `FrameSlot` data frame-local and transient. Persistent authoritative state remains owned by its defining module and is copied or projected into an immutable per-frame input only when a consumer needs it.
- Treat a thread as an execution resource, not a module boundary. Dedicated ownership contexts, shared compute work, and asynchronous input/output must remain distinct concepts.
- Derive future worker counts from a platform CPU budget and measured topology, not directly from raw logical-processor count. Do not pin threads or reserve physical cores until profiling justifies it.
- Name configurable worker resources as `ComputeThreadsGroup` and `IoThreadsGroup`, not as module-owned threads. Operating-system asynchronous I/O does not imply that an I/O threads group is occupied while a request waits.
- Keep hardware topology, platform CPU budget, and thread-group policy separate. Model topology as relations among logical processors, cores, caches, NUMA nodes, operating-system scheduling groups, and optional clusters. A core complex (CCX) is a Zen-specific source for an optional cluster, not a portable scheduler primitive.
- Never hardcode a console system-reservation count. Platform code supplies the execution budget available to the game.
- Do not read, commit, or redistribute MMD community assets without explicit permission. Record the source and license of every test asset.

## Build Rules

- Edit `premake5.lua`; never hand-edit generated solution or project files.
- Regenerate project files with `Tools/premake5.exe` after changing build targets or source-file lists.
- Do not assume build actions, compiler options, or platform names until they are defined in `premake5.lua`.

## Commit Rules

- Write commit subjects and bodies in English.
- Use an imperative, present-tense subject line no longer than 72 characters.
- Keep each commit focused on one coherent, buildable change.

## Scope Order

1. PMX binary inspection and static rendering.
2. Skeleton evaluation and GPU skinning.
3. VMD bone playback.
4. Morphs, then IK and physics.

Keep changes small, testable, and focused. Do not implement PMM, a full show editor, or physics before the earlier stages are validated.
