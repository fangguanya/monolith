---
name: wave3_impl
description: Wave 3 Discovery & Resolution implementation notes — resolve_node, search_functions, get_node_details, get_interface_functions
type: project
---

Wave 3 adds 4 actions (47 -> 51 total, plus batch_execute = 52 in skills file).

**Why:** Blueprint 47→67 plan, Wave 3 = Discovery & Resolution.

**How to apply:** Reference when extending or debugging these actions.

## Actions added

- `resolve_node` — NodeActions.h/.cpp. Creates transient node in GetTransientPackage(), calls AllocateDefaultPins(), serializes pins, calls MarkAsGarbage(). Does NOT call Graph->AddNode — the node lives on the temp graph but is never committed. Supports CallFunction, VariableGet, VariableSet, Branch, CustomEvent, Sequence. VariableGet/Set return a wildcard warning since no real BP context.
- `search_functions` — Actions.h/.cpp. Static TArray cache built on first call via TObjectIterator<UClass> + TFieldIterator<UFunction>. Skips CLASS_CompiledFromBlueprint classes. Filters FUNC_BlueprintCallable, skips DeprecatedFunction meta. cache_size returned in response. At least one of query or class_filter required (handler enforces, schema doesn't).
- `get_node_details` — Actions.h/.cpp. Uses existing FindNodeById + SerializeNode, then rebuilds pins array with bOrphanedPin added. Also adds graph_name and asset_path to response.
- `get_interface_functions` — Actions.h/.cpp. Resolves class with 3 prefix variants (bare, U prefix, strip I then add U). Iterates TFieldIterator<UFunction> with EFieldIterationFlags::None (own class only). Detects is_event via !bHasOutParms && FUNC_BlueprintEvent.

## Key gotchas found

- `resolve_node` transient graph must have Schema set (`Graph->Schema = UEdGraphSchema_K2::StaticClass()`) before AllocateDefaultPins or some node types behave differently.
- `search_functions` cache is static — built once per editor session. Class/function list is stable after module load. First call iterates thousands of classes (may be slow ~100ms), all subsequent calls are instant.
- `get_interface_functions` interface class resolution: try bare name, then U+name, then if starts with I try U+(name.Mid(1)). This handles both C++ (IMyInterface -> UMyInterface) and BP (BPI_Foo -> UBP_Foo is wrong, but bare BPI_Foo usually works for BP interfaces loaded in registry).
- `EFieldIterationFlags::None` restricts TFieldIterator to own class. `EFieldIterationFlags::IncludeSuper` would walk inherited — we intentionally exclude that for interface functions.
- `search_functions` param schema: both query and class_filter are Optional in schema. The runtime handler enforces the OR requirement and returns a clear error. This matches the spirit of Optional params better than marking query Required.

## Files modified

- `Source/MonolithBlueprint/Public/MonolithBlueprintActions.h` — 3 new handlers
- `Source/MonolithBlueprint/Private/MonolithBlueprintActions.cpp` — 3 new registrations + implementations + UObject/UObjectIterator includes
- `Source/MonolithBlueprint/Public/MonolithBlueprintNodeActions.h` — HandleResolveNode declaration
- `Source/MonolithBlueprint/Private/MonolithBlueprintNodeActions.cpp` — resolve_node registration + implementation + UObject/Package.h include
