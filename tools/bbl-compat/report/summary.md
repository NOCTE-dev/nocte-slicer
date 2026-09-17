# Bambu Studio 3MF compatibility round-trip report

- generated: `2026-09-16T21:05:25`
- corpus: `corpus`
- bambu-studio: `None`
- mode: `static-only`
- files: 19 (pass 19, fail 0)

Criteria: **a** exit code 0 - **b** no 'not from Bambu Lab' - **c** `Application` starts with `BambuStudio-` - **d** no 'newer version' warning - **e** re-export keeps objects/parts/subtypes/config keys/plates/instances - **f** paint strings kept - **g** `<ams_list>` kept. `-` means the criterion could not be evaluated in this run.

`rc` is `return_code` from the `result.json` the CLI writes into `--outputdir`; it is the only machine readable verdict Bambu Studio gives, because the binary writes no stdout and encrypts its studio log.

| file | native | exit | rc | a | b | c | d | e | f | g | notes |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `fdmhub/_PERFIL_ADD1300.3mf` | True | - | - | - | - | pass | pass | - | - | - | static-only mode: bambu-studio.exe was not invoked |
| `fdmhub/ADD-1281-Locker_ID_Tag_Rev.00_plate_1.gcode.3mf` | True | - | - | - | - | pass | pass | - | - | - | static-only mode: bambu-studio.exe was not invoked |
| `fdmhub/P10_SOPORTE_HIBRIDO_A1.3mf` | True | - | - | - | - | pass | pass | - | - | - | static-only mode: bambu-studio.exe was not invoked |
| `fdmhub/P1_TORRE_PLA_A1.3mf` | True | - | - | - | - | pass | pass | - | - | - | static-only mode: bambu-studio.exe was not invoked |
| `fdmhub/P1_TORRE_PLA_H2D.3mf` | True | - | - | - | - | pass | pass | - | - | - | static-only mode: bambu-studio.exe was not invoked |
| `fdmhub/P2_HUMEDAD_PETG_w1_A1.3mf` | True | - | - | - | - | pass | pass | - | - | - | static-only mode: bambu-studio.exe was not invoked |
| `fdmhub/P3_RETRACCION_0p6_A1.3mf` | True | - | - | - | - | pass | pass | - | - | - | static-only mode: bambu-studio.exe was not invoked |
| `fdmhub/P4_FLUJO_A1.3mf` | True | - | - | - | - | pass | pass | - | - | - | static-only mode: bambu-studio.exe was not invoked |
| `fdmhub/P4_FLUJO_A1.gcode.3mf` | True | - | - | - | - | pass | pass | - | - | - | static-only mode: bambu-studio.exe was not invoked |
| `fdmhub/P5_PRIMERA_CAPA_H2D.3mf` | True | - | - | - | - | pass | pass | - | - | - | static-only mode: bambu-studio.exe was not invoked |
| `fdmhub/P6_FLUJO_PETG_1p00_A1.3mf` | True | - | - | - | - | pass | pass | - | - | - | static-only mode: bambu-studio.exe was not invoked |
| `fdmhub/P7_SOPORTE_PLA_arbol_0p20_A1.3mf` | True | - | - | - | - | pass | pass | - | - | - | static-only mode: bambu-studio.exe was not invoked |
| `fdmhub/P8_PLANCHA_70_CRYOGRIP_PETG_A1.3mf` | True | - | - | - | - | pass | pass | - | - | - | static-only mode: bambu-studio.exe was not invoked |
| `fdmhub/P9_BARRIDO_Z2_PETG_A1.gcode.3mf` | True | - | - | - | - | pass | pass | - | - | - | static-only mode: bambu-studio.exe was not invoked |
| `fdmhub/P9_BARRIDO_Z3_PETG_A1.gcode.3mf` | True | - | - | - | - | pass | pass | - | - | - | static-only mode: bambu-studio.exe was not invoked |
| `fdmhub/P9_BARRIDO_Z_PETG_A1.gcode.3mf` | True | - | - | - | - | pass | pass | - | - | - | static-only mode: bambu-studio.exe was not invoked |
| `fdmhub/plantilla.gcode.3mf` | True | - | - | - | - | pass | pass | - | - | - | static-only mode: bambu-studio.exe was not invoked |
| `fdmhub/PRUEBA_FDMHUB.3mf` | True | - | - | - | - | pass | pass | - | - | - | static-only mode: bambu-studio.exe was not invoked |
| `fdmhub/PRUEBA_FDMHUB.gcode.3mf` | True | - | - | - | - | pass | pass | - | - | - | static-only mode: bambu-studio.exe was not invoked |

## Per-file detail

### `fdmhub/_PERFIL_ADD1300.3mf`

- size: 13448 bytes, sha256 `684586257a744614cd95d4c5026ec46bd44422f7ed917c98fa1c30050ae4c954`
- `Application`: `BambuStudio-02.08.02.60`
- sliced project: False
- objects: 0, parts: 0, subtypes: `{}`
- plater_ids: `['1']`, model_instances: 0
- paint attributes: `-`
- exit codes: `-`
- `result.json`: return_code=`None` error_string=`None`
- plaintext log available: False
- verdict: **PASS**
  - (a) skip: no Bambu Studio invocation was made
  - (b) skip: no plaintext log available (bambu-studio.exe writes no stdout and its studio log is encrypted); pass --log-file/--log-dir to decide this
  - (c) PASS: Application='BambuStudio-02.08.02.60'
  - (d) PASS: no log available, but file version 02.08.02.60 <= tested build 02.08.02.61, so no 'newer version' warning is expected
  - (e) skip: no re-export produced (use --slice)
  - (f) skip: no re-export produced (use --slice)
  - (g) skip: no re-export produced (use --slice)

### `fdmhub/ADD-1281-Locker_ID_Tag_Rev.00_plate_1.gcode.3mf`

- size: 1807850 bytes, sha256 `9a9a9cb0974e124716838356946bb493eee0a210f8e06583dbcffd4124c2d5b6`
- `Application`: `BambuStudio-02.08.02.60`
- sliced project: True
- objects: 0, parts: 0, subtypes: `{}`
- plater_ids: `['1', '2', '3']`, model_instances: 0
- paint attributes: `-`
- exit codes: `-`
- `result.json`: return_code=`None` error_string=`None`
- plaintext log available: False
- verdict: **PASS**
  - (a) skip: no Bambu Studio invocation was made
  - (b) skip: no plaintext log available (bambu-studio.exe writes no stdout and its studio log is encrypted); pass --log-file/--log-dir to decide this
  - (c) PASS: Application='BambuStudio-02.08.02.60'
  - (d) PASS: no log available, but file version 02.08.02.60 <= tested build 02.08.02.61, so no 'newer version' warning is expected
  - (e) skip: no re-export produced (use --slice)
  - (f) skip: no re-export produced (use --slice)
  - (g) skip: no re-export produced (use --slice)

### `fdmhub/P10_SOPORTE_HIBRIDO_A1.3mf`

- size: 26856 bytes, sha256 `63c2002628310101be846f98a919503ca95d239dcb6fdfd6bbb66b4173ce144b`
- `Application`: `BambuStudio-02.08.02.60`
- sliced project: False
- objects: 1, parts: 1, subtypes: `{'normal_part': 1}`
- plater_ids: `['1']`, model_instances: 1
- paint attributes: `-`
- exit codes: `-`
- `result.json`: return_code=`None` error_string=`None`
- plaintext log available: False
- verdict: **PASS**
  - (a) skip: no Bambu Studio invocation was made
  - (b) skip: no plaintext log available (bambu-studio.exe writes no stdout and its studio log is encrypted); pass --log-file/--log-dir to decide this
  - (c) PASS: Application='BambuStudio-02.08.02.60'
  - (d) PASS: no log available, but file version 02.08.02.60 <= tested build 02.08.02.61, so no 'newer version' warning is expected
  - (e) skip: no re-export produced (use --slice)
  - (f) skip: no re-export produced (use --slice)
  - (g) skip: no re-export produced (use --slice)

### `fdmhub/P1_TORRE_PLA_A1.3mf`

- size: 106121 bytes, sha256 `bce2ebf716aaab26ca9d1e5cc412429f504c35f340e78f2de3fc1c745efd975f`
- `Application`: `BambuStudio-02.08.02.60`
- sliced project: False
- objects: 1, parts: 1, subtypes: `{'normal_part': 1}`
- plater_ids: `['1']`, model_instances: 1
- paint attributes: `-`
- exit codes: `-`
- `result.json`: return_code=`None` error_string=`None`
- plaintext log available: False
- verdict: **PASS**
  - (a) skip: no Bambu Studio invocation was made
  - (b) skip: no plaintext log available (bambu-studio.exe writes no stdout and its studio log is encrypted); pass --log-file/--log-dir to decide this
  - (c) PASS: Application='BambuStudio-02.08.02.60'
  - (d) PASS: no log available, but file version 02.08.02.60 <= tested build 02.08.02.61, so no 'newer version' warning is expected
  - (e) skip: no re-export produced (use --slice)
  - (f) skip: no re-export produced (use --slice)
  - (g) skip: no re-export produced (use --slice)

### `fdmhub/P1_TORRE_PLA_H2D.3mf`

- size: 108209 bytes, sha256 `2441a706b4d841ae5f36ef5b92f88b207c22772e45402518cbf562ae22817506`
- `Application`: `BambuStudio-02.08.02.60`
- sliced project: False
- objects: 1, parts: 1, subtypes: `{'normal_part': 1}`
- plater_ids: `['1']`, model_instances: 1
- paint attributes: `-`
- exit codes: `-`
- `result.json`: return_code=`None` error_string=`None`
- plaintext log available: False
- verdict: **PASS**
  - (a) skip: no Bambu Studio invocation was made
  - (b) skip: no plaintext log available (bambu-studio.exe writes no stdout and its studio log is encrypted); pass --log-file/--log-dir to decide this
  - (c) PASS: Application='BambuStudio-02.08.02.60'
  - (d) PASS: no log available, but file version 02.08.02.60 <= tested build 02.08.02.61, so no 'newer version' warning is expected
  - (e) skip: no re-export produced (use --slice)
  - (f) skip: no re-export produced (use --slice)
  - (g) skip: no re-export produced (use --slice)

### `fdmhub/P2_HUMEDAD_PETG_w1_A1.3mf`

- size: 20513 bytes, sha256 `da5feb8ace498266e3134a3520bda3484cc7aba2cbadce78433e431aae18b653`
- `Application`: `BambuStudio-02.08.02.60`
- sliced project: False
- objects: 1, parts: 1, subtypes: `{'normal_part': 1}`
- plater_ids: `['1']`, model_instances: 1
- paint attributes: `-`
- exit codes: `-`
- `result.json`: return_code=`None` error_string=`None`
- plaintext log available: False
- verdict: **PASS**
  - (a) skip: no Bambu Studio invocation was made
  - (b) skip: no plaintext log available (bambu-studio.exe writes no stdout and its studio log is encrypted); pass --log-file/--log-dir to decide this
  - (c) PASS: Application='BambuStudio-02.08.02.60'
  - (d) PASS: no log available, but file version 02.08.02.60 <= tested build 02.08.02.61, so no 'newer version' warning is expected
  - (e) skip: no re-export produced (use --slice)
  - (f) skip: no re-export produced (use --slice)
  - (g) skip: no re-export produced (use --slice)

### `fdmhub/P3_RETRACCION_0p6_A1.3mf`

- size: 22429 bytes, sha256 `2b0da95083608652cf292ac668150e730a6a754a51201539949e0ad224857856`
- `Application`: `BambuStudio-02.08.02.60`
- sliced project: False
- objects: 1, parts: 1, subtypes: `{'normal_part': 1}`
- plater_ids: `['1']`, model_instances: 1
- paint attributes: `-`
- exit codes: `-`
- `result.json`: return_code=`None` error_string=`None`
- plaintext log available: False
- verdict: **PASS**
  - (a) skip: no Bambu Studio invocation was made
  - (b) skip: no plaintext log available (bambu-studio.exe writes no stdout and its studio log is encrypted); pass --log-file/--log-dir to decide this
  - (c) PASS: Application='BambuStudio-02.08.02.60'
  - (d) PASS: no log available, but file version 02.08.02.60 <= tested build 02.08.02.61, so no 'newer version' warning is expected
  - (e) skip: no re-export produced (use --slice)
  - (f) skip: no re-export produced (use --slice)
  - (g) skip: no re-export produced (use --slice)

### `fdmhub/P4_FLUJO_A1.3mf`

- size: 12514 bytes, sha256 `fd494f226fc74e524713cc0beaa187d44f2a442c80df654d770965f16b9bad8c`
- `Application`: `BambuStudio-02.08.02.60`
- sliced project: False
- objects: 1, parts: 1, subtypes: `{'normal_part': 1}`
- plater_ids: `['1']`, model_instances: 1
- paint attributes: `-`
- exit codes: `-`
- `result.json`: return_code=`None` error_string=`None`
- plaintext log available: False
- verdict: **PASS**
  - (a) skip: no Bambu Studio invocation was made
  - (b) skip: no plaintext log available (bambu-studio.exe writes no stdout and its studio log is encrypted); pass --log-file/--log-dir to decide this
  - (c) PASS: Application='BambuStudio-02.08.02.60'
  - (d) PASS: no log available, but file version 02.08.02.60 <= tested build 02.08.02.61, so no 'newer version' warning is expected
  - (e) skip: no re-export produced (use --slice)
  - (f) skip: no re-export produced (use --slice)
  - (g) skip: no re-export produced (use --slice)

### `fdmhub/P4_FLUJO_A1.gcode.3mf`

- size: 63131 bytes, sha256 `11acfa8b3aa49df74fae09da2f75f8772d5363b47796849cf31564a3893682b7`
- `Application`: `BambuStudio-02.04.00.70`
- sliced project: True
- objects: 0, parts: 0, subtypes: `{}`
- plater_ids: `['1']`, model_instances: 0
- paint attributes: `-`
- exit codes: `-`
- `result.json`: return_code=`None` error_string=`None`
- plaintext log available: False
- verdict: **PASS**
  - (a) skip: no Bambu Studio invocation was made
  - (b) skip: no plaintext log available (bambu-studio.exe writes no stdout and its studio log is encrypted); pass --log-file/--log-dir to decide this
  - (c) PASS: Application='BambuStudio-02.04.00.70'
  - (d) PASS: no log available, but file version 02.04.00.70 <= tested build 02.08.02.61, so no 'newer version' warning is expected
  - (e) skip: no re-export produced (use --slice)
  - (f) skip: no re-export produced (use --slice)
  - (g) skip: no re-export produced (use --slice)

### `fdmhub/P5_PRIMERA_CAPA_H2D.3mf`

- size: 14986 bytes, sha256 `e5c7dc56808297cb92067dc1d8c6f3dc953fda15faef60df2a4c43e83b61ff70`
- `Application`: `BambuStudio-02.08.02.60`
- sliced project: False
- objects: 1, parts: 1, subtypes: `{'normal_part': 1}`
- plater_ids: `['1']`, model_instances: 1
- paint attributes: `-`
- exit codes: `-`
- `result.json`: return_code=`None` error_string=`None`
- plaintext log available: False
- verdict: **PASS**
  - (a) skip: no Bambu Studio invocation was made
  - (b) skip: no plaintext log available (bambu-studio.exe writes no stdout and its studio log is encrypted); pass --log-file/--log-dir to decide this
  - (c) PASS: Application='BambuStudio-02.08.02.60'
  - (d) PASS: no log available, but file version 02.08.02.60 <= tested build 02.08.02.61, so no 'newer version' warning is expected
  - (e) skip: no re-export produced (use --slice)
  - (f) skip: no re-export produced (use --slice)
  - (g) skip: no re-export produced (use --slice)

### `fdmhub/P6_FLUJO_PETG_1p00_A1.3mf`

- size: 12534 bytes, sha256 `292eeb75d549fdd8dcdd384b3ecc490b8f9960ac33fcdeba22b711744bb49b31`
- `Application`: `BambuStudio-02.08.02.60`
- sliced project: False
- objects: 1, parts: 1, subtypes: `{'normal_part': 1}`
- plater_ids: `['1']`, model_instances: 1
- paint attributes: `-`
- exit codes: `-`
- `result.json`: return_code=`None` error_string=`None`
- plaintext log available: False
- verdict: **PASS**
  - (a) skip: no Bambu Studio invocation was made
  - (b) skip: no plaintext log available (bambu-studio.exe writes no stdout and its studio log is encrypted); pass --log-file/--log-dir to decide this
  - (c) PASS: Application='BambuStudio-02.08.02.60'
  - (d) PASS: no log available, but file version 02.08.02.60 <= tested build 02.08.02.61, so no 'newer version' warning is expected
  - (e) skip: no re-export produced (use --slice)
  - (f) skip: no re-export produced (use --slice)
  - (g) skip: no re-export produced (use --slice)

### `fdmhub/P7_SOPORTE_PLA_arbol_0p20_A1.3mf`

- size: 12652 bytes, sha256 `7ebb8c5b90081e627098992a4153393281d1558243265a2668406a7e094781d0`
- `Application`: `BambuStudio-02.08.02.60`
- sliced project: False
- objects: 1, parts: 1, subtypes: `{'normal_part': 1}`
- plater_ids: `['1']`, model_instances: 1
- paint attributes: `-`
- exit codes: `-`
- `result.json`: return_code=`None` error_string=`None`
- plaintext log available: False
- verdict: **PASS**
  - (a) skip: no Bambu Studio invocation was made
  - (b) skip: no plaintext log available (bambu-studio.exe writes no stdout and its studio log is encrypted); pass --log-file/--log-dir to decide this
  - (c) PASS: Application='BambuStudio-02.08.02.60'
  - (d) PASS: no log available, but file version 02.08.02.60 <= tested build 02.08.02.61, so no 'newer version' warning is expected
  - (e) skip: no re-export produced (use --slice)
  - (f) skip: no re-export produced (use --slice)
  - (g) skip: no re-export produced (use --slice)

### `fdmhub/P8_PLANCHA_70_CRYOGRIP_PETG_A1.3mf`

- size: 12519 bytes, sha256 `34ff5d57da98a647aa3061453aba13a01536de30d203b56f7b32e91ea086b9f1`
- `Application`: `BambuStudio-02.08.02.60`
- sliced project: False
- objects: 1, parts: 1, subtypes: `{'normal_part': 1}`
- plater_ids: `['1']`, model_instances: 1
- paint attributes: `-`
- exit codes: `-`
- `result.json`: return_code=`None` error_string=`None`
- plaintext log available: False
- verdict: **PASS**
  - (a) skip: no Bambu Studio invocation was made
  - (b) skip: no plaintext log available (bambu-studio.exe writes no stdout and its studio log is encrypted); pass --log-file/--log-dir to decide this
  - (c) PASS: Application='BambuStudio-02.08.02.60'
  - (d) PASS: no log available, but file version 02.08.02.60 <= tested build 02.08.02.61, so no 'newer version' warning is expected
  - (e) skip: no re-export produced (use --slice)
  - (f) skip: no re-export produced (use --slice)
  - (g) skip: no re-export produced (use --slice)

### `fdmhub/P9_BARRIDO_Z2_PETG_A1.gcode.3mf`

- size: 61727 bytes, sha256 `4ce530506356cd6923a134a21ae3e93a131031c17cc4c07dfc2e842a6d0e4c14`
- `Application`: `BambuStudio-02.08.02.60`
- sliced project: True
- objects: 0, parts: 0, subtypes: `{}`
- plater_ids: `['1']`, model_instances: 0
- paint attributes: `-`
- exit codes: `-`
- `result.json`: return_code=`None` error_string=`None`
- plaintext log available: False
- verdict: **PASS**
  - (a) skip: no Bambu Studio invocation was made
  - (b) skip: no plaintext log available (bambu-studio.exe writes no stdout and its studio log is encrypted); pass --log-file/--log-dir to decide this
  - (c) PASS: Application='BambuStudio-02.08.02.60'
  - (d) PASS: no log available, but file version 02.08.02.60 <= tested build 02.08.02.61, so no 'newer version' warning is expected
  - (e) skip: no re-export produced (use --slice)
  - (f) skip: no re-export produced (use --slice)
  - (g) skip: no re-export produced (use --slice)

### `fdmhub/P9_BARRIDO_Z3_PETG_A1.gcode.3mf`

- size: 61726 bytes, sha256 `a66f4e32f5b0d746b379c3dce99160a7c3767392747cd35d486a5734b882abff`
- `Application`: `BambuStudio-02.08.02.60`
- sliced project: True
- objects: 0, parts: 0, subtypes: `{}`
- plater_ids: `['1']`, model_instances: 0
- paint attributes: `-`
- exit codes: `-`
- `result.json`: return_code=`None` error_string=`None`
- plaintext log available: False
- verdict: **PASS**
  - (a) skip: no Bambu Studio invocation was made
  - (b) skip: no plaintext log available (bambu-studio.exe writes no stdout and its studio log is encrypted); pass --log-file/--log-dir to decide this
  - (c) PASS: Application='BambuStudio-02.08.02.60'
  - (d) PASS: no log available, but file version 02.08.02.60 <= tested build 02.08.02.61, so no 'newer version' warning is expected
  - (e) skip: no re-export produced (use --slice)
  - (f) skip: no re-export produced (use --slice)
  - (g) skip: no re-export produced (use --slice)

### `fdmhub/P9_BARRIDO_Z_PETG_A1.gcode.3mf`

- size: 61724 bytes, sha256 `98b5fb04b4f6a1069ac6abd1a2c164576e531fbdb264c88f219d3af389155dcf`
- `Application`: `BambuStudio-02.08.02.60`
- sliced project: True
- objects: 0, parts: 0, subtypes: `{}`
- plater_ids: `['1']`, model_instances: 0
- paint attributes: `-`
- exit codes: `-`
- `result.json`: return_code=`None` error_string=`None`
- plaintext log available: False
- verdict: **PASS**
  - (a) skip: no Bambu Studio invocation was made
  - (b) skip: no plaintext log available (bambu-studio.exe writes no stdout and its studio log is encrypted); pass --log-file/--log-dir to decide this
  - (c) PASS: Application='BambuStudio-02.08.02.60'
  - (d) PASS: no log available, but file version 02.08.02.60 <= tested build 02.08.02.61, so no 'newer version' warning is expected
  - (e) skip: no re-export produced (use --slice)
  - (f) skip: no re-export produced (use --slice)
  - (g) skip: no re-export produced (use --slice)

### `fdmhub/plantilla.gcode.3mf`

- size: 670685 bytes, sha256 `dda74763c1660736b3065286e8a38043b6468c865cc17d0959e2f98af2045808`
- `Application`: `BambuStudio-02.04.00.70`
- sliced project: True
- objects: 0, parts: 0, subtypes: `{}`
- plater_ids: `['1']`, model_instances: 0
- paint attributes: `-`
- exit codes: `-`
- `result.json`: return_code=`None` error_string=`None`
- plaintext log available: False
- verdict: **PASS**
  - (a) skip: no Bambu Studio invocation was made
  - (b) skip: no plaintext log available (bambu-studio.exe writes no stdout and its studio log is encrypted); pass --log-file/--log-dir to decide this
  - (c) PASS: Application='BambuStudio-02.04.00.70'
  - (d) PASS: no log available, but file version 02.04.00.70 <= tested build 02.08.02.61, so no 'newer version' warning is expected
  - (e) skip: no re-export produced (use --slice)
  - (f) skip: no re-export produced (use --slice)
  - (g) skip: no re-export produced (use --slice)

### `fdmhub/PRUEBA_FDMHUB.3mf`

- size: 47201 bytes, sha256 `74b93593d6614170ac34683bd4e6b7488893243c000180972c2a51e34396c39c`
- `Application`: `BambuStudio-02.08.02.60`
- sliced project: False
- objects: 1, parts: 1, subtypes: `{'normal_part': 1}`
- plater_ids: `['1']`, model_instances: 1
- paint attributes: `-`
- exit codes: `-`
- `result.json`: return_code=`None` error_string=`None`
- plaintext log available: False
- verdict: **PASS**
  - (a) skip: no Bambu Studio invocation was made
  - (b) skip: no plaintext log available (bambu-studio.exe writes no stdout and its studio log is encrypted); pass --log-file/--log-dir to decide this
  - (c) PASS: Application='BambuStudio-02.08.02.60'
  - (d) PASS: no log available, but file version 02.08.02.60 <= tested build 02.08.02.61, so no 'newer version' warning is expected
  - (e) skip: no re-export produced (use --slice)
  - (f) skip: no re-export produced (use --slice)
  - (g) skip: no re-export produced (use --slice)

### `fdmhub/PRUEBA_FDMHUB.gcode.3mf`

- size: 101407 bytes, sha256 `e8fa968a18feed7a142a84b6d9b0b0bbdf313c61068601ed092cd1bdf52ed575`
- `Application`: `BambuStudio-02.04.00.70`
- sliced project: True
- objects: 0, parts: 0, subtypes: `{}`
- plater_ids: `['1']`, model_instances: 0
- paint attributes: `-`
- exit codes: `-`
- `result.json`: return_code=`None` error_string=`None`
- plaintext log available: False
- verdict: **PASS**
  - (a) skip: no Bambu Studio invocation was made
  - (b) skip: no plaintext log available (bambu-studio.exe writes no stdout and its studio log is encrypted); pass --log-file/--log-dir to decide this
  - (c) PASS: Application='BambuStudio-02.04.00.70'
  - (d) PASS: no log available, but file version 02.04.00.70 <= tested build 02.08.02.61, so no 'newer version' warning is expected
  - (e) skip: no re-export produced (use --slice)
  - (f) skip: no re-export produced (use --slice)
  - (g) skip: no re-export produced (use --slice)
