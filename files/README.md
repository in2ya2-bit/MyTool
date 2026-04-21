# Level Tool — UE5 Real-World Map Generator

Extraction Shooter용 실사 지형·건물 자동 생성 툴.  
구글 맵 / OSM 데이터로 UE5 Landscape + Static Mesh Actor를 원클릭 생성합니다.

---

## 구조

```
level_tool/
├── config.py                         좌표 프리셋, 스케일 상수
├── main.py                           CLI 진입점
│
├── landscape/
│   ├── elevation_fetcher.py          고도 데이터 취득
│   │   ├── Open-Elevation  (무료, SRTM 30m)
│   │   ├── OpenTopography  (무료 키, SRTM)
│   │   └── Google Maps API (유료, 최고 정확도)
│   └── heightmap_exporter.py         Heightmap 처리 + UE5 에셋 출력
│       ├── Gaussian smooth
│       ├── 수력 침식 시뮬레이션
│       ├── 16-bit PNG / R16 출력
│       └── Splat map 4종 (grass/rock/sand/snow)
│
├── buildings/
│   ├── osm_fetcher.py                Overpass API → 건물 파싱
│   │   ├── 풋프린트 폴리곤 + 높이 추출
│   │   ├── 건물 유형 분류 (11종)
│   │   └── UE5 좌표계 변환 (EPSG:4326 → cm XY)
│   └── blender_extrude.py            Blender 내부 실행 → FBX 압출
│       ├── OSM 폴리곤 → 3D 메쉬
│       ├── UV 언랩 (벽/지붕 분리)
│       ├── 타입별 머티리얼 자동 할당
│       └── 타입별 FBX 그룹 출력
│
├── ue5/
│   └── level_builder.py              UE5 Python 콘솔 자동화
│       ├── Heightmap → Landscape Actor 임포트
│       ├── Buildings JSON → Actor 배치
│       └── PCG CSV 출력
│
└── ue5_plugin/LevelTool/             UE5 C++ 에디터 플러그인 (Phase 3)
    ├── LevelTool.uplugin
    └── Source/LevelTool/
        ├── Public/
        │   ├── LevelToolSettings.h   UObject 설정 (Project Settings 통합)
        │   ├── LevelToolBuildingPool.h  건물 메쉬 풀 DataAsset
        │   ├── LevelToolSubsystem.h  EditorSubsystem (Python 실행 + 임포트)
        │   ├── SLevelToolPanel.h     Slate UI 패널 헤더
        │   └── LevelToolModule.h     모듈 등록
        └── Private/
            ├── LevelToolSettings.cpp
            ├── LevelToolSubsystem.cpp
            ├── SLevelToolPanel.cpp   전체 UI 구현
            └── LevelToolModule.cpp   탭/메뉴/설정 등록
```

---

## Python 툴 빠른 시작

### 설치
```bash
pip install requests numpy pillow scipy
```

### 사용법

```bash
# 서울 종로 전체 파이프라인 (무료 SRTM)
python main.py all --preset Seoul_Jongno

# Chernobyl 고정밀 지형 (Google API)
python main.py landscape --preset Chernobyl \
    --elevation-source google --api-key YOUR_KEY

# 인천 항구 건물만
python main.py buildings --lat 37.4536 --lon 126.7020 --radius 1.5

# 사용 가능한 프리셋 목록
python main.py presets
```

### 출력 파일

| 파일 | 용도 |
|------|------|
| `output/heightmaps/<name>_heightmap.png` | UE5 Import Landscape → From File |
| `output/heightmaps/<name>_heightmap.r16` | UE5 Raw 16-bit 임포트 대안 |
| `output/heightmaps/<name>_splat_grass.png` | Landscape Material 레이어 |
| `output/heightmaps/<name>_splat_rock.png`  | Landscape Material 레이어 |
| `output/heightmaps/<name>_splat_sand.png`  | Landscape Material 레이어 |
| `output/heightmaps/<name>_splat_snow.png`  | Landscape Material 레이어 |
| `output/buildings/<name>_buildings.json`   | 건물 좌표+메타데이터 |
| `output/ue5_scripts/<name>_pcg.csv`        | PCG AttributeSet 임포트용 |

---

## Blender FBX 압출

```bash
# Blender 헤드리스 실행 (buildings.json → FBX 일괄 출력)
blender --background --python buildings/blender_extrude.py \
    -- output/buildings/Seoul_Jongno_buildings.json

# 출력: output/buildings/fbx/
#   buildings_combined.fbx       (전체 통합)
#   BP_Building_Residential.fbx  (타입별)
#   BP_Building_Commercial.fbx
#   ...
```

---

## UE5 Python 콘솔 자동화

UE5 에디터 → Tools → Execute Python Script, 또는 Python 콘솔에서:

```python
import sys
sys.path.insert(0, r"C:/Projects/level_tool")

from ue5.level_builder import run_full_pipeline

run_full_pipeline(
    heightmap_path = r"C:/Projects/level_tool/output/heightmaps/Seoul_Jongno_heightmap.png",
    buildings_json = r"C:/Projects/level_tool/output/buildings/Seoul_Jongno_buildings.json",
    map_name       = "Seoul_Jongno",
    xy_scale       = 100.0,    # 1m per quad
    z_range_cm     = 40000.0,  # 400m total height range
)
```

---

## UE5 C++ 플러그인 설치 — Engine Plugin 모드 (권장)

이 플러그인은 엔진 설치본에 한 번만 깔아두면 **그 엔진을 쓰는 모든 UE5 프로젝트에서
자동 enable** 되도록 설계돼 있습니다 (`LevelTool.uplugin` 에 `EnabledByDefault: true`).
따라서 사용하려는 프로젝트의 `.uproject` / `Config` 를 **한 글자도 건드릴 필요가
없어서** Perforce 관리 프로젝트에도 부담 없이 붙일 수 있습니다.

### 설치

리포 루트에서 PowerShell:

```powershell
# 이 리포가 엔진 루트 바로 밑에 있을 때 (예: E:\WorkUE5\MyTool)
.\install_to_engine.ps1

# 엔진 경로가 다르면 명시
.\install_to_engine.ps1 -EnginePath "E:\UE5\UE_5.5"

# 이전 설치본 덮어쓰기
.\install_to_engine.ps1 -Force
```

스크립트가 하는 일:
1. `Plugins\LevelTool\` 을 `<Engine>\Engine\Plugins\Marketplace\LevelTool\` 로 복사
2. `files\` (Python 파이프라인 전체) 를 `<...>\LevelTool\files\` 로 복사
3. 이후 `ULevelToolSubsystem` 이 `PluginBaseDir/files` 를 자동 탐지하므로
   `Project Settings → Level Tool → Python Script Directory` 를 비워둬도 동작

### 첫 사용

1. 에디터가 열려 있다면 닫습니다.
2. 사용하려는 `.uproject` 를 우클릭 → **Generate Visual Studio project files**
3. 에디터를 열면 "Modules missing" 프롬프트가 뜨며 LevelTool 모듈 빌드가 한 번 수행됩니다.
4. 이후부터는 메뉴 **Tools → Level Tool** 이 어떤 프로젝트에서든 노출됩니다.

### API 키/출력 경로 설정 (선택)

- `Edit → Project Settings → Plugins → Level Tool` 에서 지정
- Python Script Directory: **비워두면** 자동 탐지됨 (엔진 설치본 내부의 files/)
- Output Directory: 하이트맵/JSON 산출물을 둘 경로 (기본 `<Project>/Saved/LevelTool/` 사용)
- API Keys: OpenTopography / Google Maps 쓸 때만

### 제거

```powershell
.\uninstall_from_engine.ps1
```

### 프로젝트별 설치가 필요하다면

위 스크립트 대신 `Plugins\LevelTool\` 폴더를 대상 프로젝트의 `Plugins\` 에
직접 복사해도 됩니다. 이 경우는 `.uproject` 가 plugin entry 를 자동 기록하므로
Perforce 에 올릴 때 diff 가 발생하는 점 주의.

### 패널 사용법

1. **Location** — 프리셋 선택 또는 위도/경도/반경 직접 입력
2. **Options** — Landscape / Buildings 체크, Elevation source 선택, Building Pool DataAsset 연결
3. **▶ Generate Map** 클릭
4. 진행 상황 로그에서 확인

---

## Private 워크플로우 — 툴은 로컬, 맵만 Perforce 서밋

이 플러그인을 **엔진 설치본에 깔아두고** (위의 Engine Plugin 모드), Perforce 로 관리되는
프로젝트 안에서 바로 맵을 만든 다음, `/Game/MapData/<MapName>/` 한 폴더만 서밋하면 되는
절차입니다. 플러그인 소스/바이너리는 어디에도 add 되지 않습니다.

### 사전 조건

- `install_to_engine.ps1` 로 엔진 설치 완료 상태
- 타겟 프로젝트의 `.uproject` / `Config/DefaultEngine.ini` 등은 전혀 수정하지 않음
  (`EnabledByDefault: true` 덕분에 따로 enable 할 필요 없음)
- `Edit > Project Settings > Plugins > Level Tool` 에서 Output Directory / API Keys 만
  프로젝트별로 지정해주면 됨 (이 `DefaultEditorPerProjectUserSettings.ini` 는 Perforce 에
  안 올리는 파일이라 안전합니다)

### 0. 맵 생성

**타겟 프로젝트 에디터** (예: TslGame) 를 열고 `Tools → Level Tool` 패널에서
`▶ Generate Map`. Landscape / Buildings / Roads / Water 가 현재 월드에 바로 생성됩니다.

### 1. Finalize for submit 섹션에서 맵 이름 입력

예: `SeoulDemo`, `Erangel_v2`. `/Game/MapData/<이름>/` 루트가 됩니다.

### 2. `1. Bake Roads → StaticMesh`

도로는 `UProceduralMeshComponent` 로 런타임 구성돼 있습니다. Perforce 에 그대로 올릴
수는 있지만, 장기 보관/리빌드 안정성을 위해 `UStaticMesh` 에셋으로 구워두는 편이 안전:

- 모든 `LevelTool/Roads` 폴더 PMC 액터를 `UStaticMesh` 로 굽고
  `/Game/MapData/<MapName>/Roads/SM_*` 에 저장
- 원본 PMC 액터는 `AStaticMeshActor` 로 교체 (머티리얼 유지)

### 3. `2. Cleanup for Migrate`

- `LevelTool/Compass` 폴더의 기둥 + 라벨(N/S/E/W/CENTER) 제거
- `LevelTool/` 하위의 디버그 `ATextRenderActor` 제거

### 4. `3. Pack assets under /Game/MapData/<Name>/`

런타임 생성 머티리얼/텍스처를 `/Game/MapData/<MapName>/FromLevelTool/` 로 리네임.
레벨의 참조는 AssetTools 가 자동 fixup. 결과적으로 이 맵이 쓰는 에셋 전부가
`/Game/MapData/<MapName>/` 한 폴더에 모입니다.

### 5. 레벨 저장 + Perforce 서밋

1. `File > Save Current Level As...` → `/Game/MapData/<MapName>/<MapName>.umap`
2. Source Control 에서 Reconcile 또는 P4V 에서 **Reconcile Offline Work**
3. 새 파일들이 `Content/MapData/<MapName>/...` 아래에만 몰려있는지 확인
4. 체인지리스트 서밋

> 플러그인 자체 (`Engine/Plugins/Marketplace/LevelTool/`) 는 엔진 설치본 안에 있어서
> 프로젝트 Perforce 워크스페이스가 절대 touch 하지 않습니다. 자동으로 "로컬 전용" 이 됩니다.

### 다른 프로젝트로 옮기고 싶다면 (선택)

같은 엔진 버전이면 Content Browser `/Game/MapData/<MapName>/<MapName>.umap` 우클릭 →
**Asset Actions > Migrate...** 로 타겟 프로젝트 Content 폴더에 통째로 이식 가능.
Step 4 를 거쳤기 때문에 migrate 의존성 트리에 `/Game/LevelTool/` 잔재가 남지 않습니다.

---

## UE5 Landscape 임포트 수동 설정값

| 항목 | 값 | 비고 |
|------|-----|------|
| Heightmap file | `*_heightmap.png` | 16-bit Grayscale PNG |
| XY Scale | `100.0` | 1m per landscape quad |
| Z Scale | `78.125` | 400m 범위 기준 (40000/512) |
| Section size | `63 quads` | 1009×1009 그리드용 |
| Sections per component | `1` | |

---

## 건물 메쉬 풀 설정

UE5 Content Browser에서 `LevelToolBuildingPool` DataAsset 생성:
- `/Game/LevelTool/Data/DA_BuildingPool`
- 각 TypeKey에 Static Mesh 연결
- FallbackMesh 반드시 설정

| TypeKey | 설명 |
|---------|------|
| `BP_Building_Residential` | 주거용 단독주택 |
| `BP_Building_Apartment`   | 아파트 |
| `BP_Building_Commercial`  | 상업용 건물 |
| `BP_Building_Industrial`  | 공장/창고 |
| `BP_Building_Office`      | 오피스 |
| `BP_Building_Generic`     | 분류 불명 (Fallback) |

---

## 단계별 구현 로드맵

| Phase | 상태 | 내용 |
|-------|------|------|
| 1 | ✅ 완료 | Python 스탠드얼론 파이프라인 |
| 2 | ✅ 완료 | UE5 Python 자동화 스크립트 |
| 3 | ✅ 완료 | UE5 C++ Slate 에디터 플러그인 |
| 4 | 🔜 예정 | AI 강화 (TripoSR 메쉬 품질, SD 텍스처 자동생성) |
