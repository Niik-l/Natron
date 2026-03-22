# Dev Nodes — Development Notes

Technical notes and gotchas discovered during development of the custom node extensions.

---

## Node Lifecycle: getNodes() vs getNodes_recursive()

**Important:** When scanning for active nodes (e.g., checking if a DeepToPoints exists), always use:

```cpp
NodesList nodes;
project->getNodes_recursive(nodes, true);  // true = only active nodes
```

**Do NOT use:**
```cpp
NodesList nodes = project->getNodes();  // Returns ALL nodes including deleted/deactivated!
```

**Why:** Natron keeps deleted nodes in memory (deactivated but not destroyed) to support undo/redo. `getNodes()` returns these deactivated nodes too. `getNodes_recursive(nodes, true)` filters to only active nodes.

**Also add this check for extra safety:**
```cpp
if (!(*it)->isActivated()) continue;
```

**Discovered in:** Viewport3D point cloud persistence bug — deleted DeepToPoints nodes were still found by the viewport's scan because `getNodes()` returned deactivated nodes, causing stale point cloud data to persist indefinitely.

---

## Point Cloud Data Lifecycle

The `PointCloudData` shared_ptr can outlive the DeepToPoints node that created it because:
1. `DeepToPoints::_lastPointCloud` holds a shared_ptr (cleared in destructor)
2. `Viewport3D::_imp->pointCloud` holds another shared_ptr (must be cleared when node is removed)
3. Natron's undo system keeps the node object alive even after "deletion"

**Fix pattern:** Clear cached data unconditionally each frame, then only re-set it if an active source node exists:
```cpp
// Always clear first
_imp->pointCloud.reset();

// Then scan for active nodes
NodesList nodes;
project->getNodes_recursive(nodes, true);
for (...) {
    if (!(*it)->isActivated()) continue;
    // ... find and set point cloud ...
}
```

---

## Qt6 Compatibility Notes for Dev Nodes

When writing new nodes, watch for these Qt6 differences:

| Pattern | Qt5 | Qt6 |
|---------|-----|-----|
| Enter event | `enterEvent(QEvent*)` | `enterEvent(QtCompat::QEnterEvent*)` |
| Key combos | `Qt::CTRL + Qt::Key_X` | `Qt::CTRL \| Qt::Key_X` |
| Mutex locker | `QMutexLocker` | Works via CTAD, but `unique_ptr<QMutexLocker>` needs `QtMutexLocker` typedef |
| QRecursiveMutex | Same as QMutex | Separate type, needs `QMutexLocker<QRecursiveMutex>` |
| QChar::unicode() | Returns `ushort` | Returns `char16_t`, cast to `int` for `arg()` |

---

## Build Dependencies

| Node Category | External Dependencies |
|--------------|----------------------|
| Deep nodes | OpenImageIO (optional, enables EXR deep reading) |
| 3D/Volume | OpenVDB (optional, for ReadVDB) |
| 3D/Geometry | Alembic (optional, for ReadAlembicCamera/ReadGeo) |

CMake flags:
- `NATRON_HAVE_OPENIMAGEIO` — defined automatically when OpenImageIO is found
- OpenVDB and Alembic support is conditional in the node code via `#ifdef`
