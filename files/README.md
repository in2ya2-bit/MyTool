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

## UE5 C++ 플러그인 설치

1. `ue5_plugin/LevelTool/` 폴더를 UE5 프로젝트의 `Plugins/` 에 복사
2. `.uproject` 오른쪽 클릭 → Generate Visual Studio project files
3. 빌드 후 에디터 실행
4. `Edit → Plugins` 에서 "Level Tool" 활성화
5. `Edit → Project Settings → Plugins → Level Tool` 에서 설정:
   - Python Script Directory: `level_tool/` 폴더 경로
   - Output Directory: 출력 폴더 경로
   - API Keys (선택)
6. `Tools → Level Tool` 으로 패널 열기

### 패널 사용법

1. **Location** — 프리셋 선택 또는 위도/경도/반경 직접 입력
2. **Options** — Landscape / Buildings 체크, Elevation source 선택, Building Pool DataAsset 연결
3. **▶ Generate Map** 클릭
4. 진행 상황 로그에서 확인

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
