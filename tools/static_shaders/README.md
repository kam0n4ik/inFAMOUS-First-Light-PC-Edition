# Static shader prediction — research tools and templates

The material draws' VS/PS modules predicted from the game's packs, without running the game
(`docs\REA_ENGINE_REVERSE.md` §8). The C++ side is `src\runtime\gpu\static\` and
`SecondSonPC.exe --predict-shaders <templates>`; these scripts found and checked the layouts it uses and make its
template file. They only read the game files (`E:\shdps4\CUSA00004`) and earlier runs' logs and caches.

Dependencies: Python 3.13 (`py -I -X utf8`), the KCAP reader `E:\SecondSonPC-rebirth\native\tools\research\kcap.py`,
`tools\p414\pack_analyze.py` (shader cache records).

| script | what |
|---|---|
| `material_db.py` | program objects, passes, material instances, textures of every pack -> JSON |
| `shader_structs.py` | the passes' Gnmx VS/PS structs (registers, usage slots, semantics) -> JSON |
| `validate_material.py` | material textures and stage codes against an earlier binding log |
| `check_fs_runtime.py` | the fragment RuntimeParams from the structs against cached pipelines |
| `predict_runtime.py` | render state by pass class against the caches (STATE_ONLY=1) |
| `templates.py` | named textures / buffer resources, the constant buffer, samplers -> template lines |
| `state_table.py` | render state by (role, pass class, COL_FORMAT) -> template lines (appended) |
| `predict_loop.ps1` | runs `--predict-shaders` until it completes (a recompiler assertion: skip list + resume) |
| `compare_predict.py` | the prediction report against scenes' caches (layout / keyed hits) |
| `keyed_miss.py` | why a keyed module was missed (the differing key words) |

`static_templates.txt`: the template file made from the binding log `bindings_v1.jsonl` and the caches of three
scenes (`logs\p414\m1\cache`): `tex`/`rbuf`/`cb`/`smp`/`state` lines (formats in `static_predictor.cpp`,
`StaticTemplates::Load`).

Make it again:

```
py -I -X utf8 material_db.py out\material_db.json E:\shdps4\CUSA00004
py -I -X utf8 templates.py out\material_db.json out\pack_shaders.json out\bindings_v1.jsonl static_templates.txt
py -I -X utf8 state_table.py out\material_db.json static_templates.txt <scene shaders.pack>...
```

Run it into the normal shader cache (then a game start finds the modules):

```
build\bin\SecondSonPC.exe --gpu-backend sspc --predict-shaders tools\static_shaders\static_templates.txt
```
