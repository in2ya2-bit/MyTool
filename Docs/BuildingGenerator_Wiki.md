# BuildingGenerator - UE5 건물 생성 플러그인

## 기획 의도

### 목표
레벨 디자이너가 **코드 없이** 에디터 내에서 건물 구조를 설계하고, 즉시 3D 결과물을 확인할 수 있는 타일 기반 건물 생성 도구.

### 해결하려는 문제
| 기존 방식 | BuildingGenerator |
|:---|:---|
| 벽/바닥을 하나씩 수동 배치 | 2D 타일맵에서 클릭으로 구조 설계 |
| 메쉬 교체 시 전체 수작업 | DataAsset 교체로 일괄 반영 |
| 스케일 변경 시 재배치 필요 | BuildingScale 슬라이더로 즉시 조정 |
| 반복 구조 복사 번거로움 | 층 복사/붙여넣기로 즉시 복제 |

### 핵심 컨셉
- **타일 기반 설계**: 2D 그리드에서 벽, 바닥, 문, 창문, 계단 등을 페인트
- **AutoTile**: 인접 벽을 자동 감지하여 직선/코너/T자/십자 변형 선택 및 회전
- **HISM 렌더링**: Hierarchical Instanced Static Mesh로 대량 인스턴스 최적화
- **런타임 스케일 보정**: 메쉬 임포트 크기를 자동 감지하여 타일 크기에 맞춤

---

## 구현 내용

### 플러그인 구조

```
Plugins/BuildingGenerator/
├── Source/
│   ├── BuildingGenerator/          ← Runtime 모듈
│   │   ├── Public/
│   │   │   ├── BuildingTypes.h         # 핵심 데이터 구조
│   │   │   ├── BuildingActor.h         # HISM 기반 건물 액터
│   │   │   ├── BuildingDataAsset.h     # 메쉬 매핑 데이터에셋
│   │   │   ├── BuildingAutoTile.h      # 벽 자동 변형 시스템
│   │   │   └── BuildingModuleActor.h   # 개별 타일 편집 프록시
│   │   └── Private/
│   │       └── *.cpp
│   └── BuildingGeneratorEditor/    ← Editor 모듈
│       ├── Public/
│       │   ├── SBuildingEditorWindow.h # 메인 에디터 윈도우
│       │   ├── STilemapGrid.h          # 2D 타일맵 위젯
│       │   └── BuildingActorCustomization.h
│       └── Private/
│           └── *.cpp
```

### 데이터 구조

#### ETileType (12종)
| 타일 | 설명 |
|:---|:---|
| Empty | 빈 공간 |
| Floor | 일반 바닥 |
| Wall / Wall_Door / Wall_Window | 벽 계열 (AutoTile 대상) |
| Room_A / Room_B / Room_C | 방 구분 (바닥 + 색상 구분) |
| Corridor | 복도 |
| Stairs_Up / Stairs_Down | 계단 |

#### EWallVariant (AutoTile 결과, 6종)
4-bit 이웃 마스크 (N=0001, S=0010, E=0100, W=1000) → 16가지 조합 → 6 변형

| 변형 | 마스크 예시 | 설명 |
|:---|:---|:---|
| Isolated | 0000 | 독립 벽 |
| End | 0001 등 | 끝 벽 |
| Straight | 0011, 1100 | 직선 |
| Corner | 0101 등 | 코너 |
| T_Junction | 0111 등 | T자 |
| Cross | 1111 | 십자 |

#### FBuildingData (건물 전체)
```
FBuildingData
├── TArray<FFloorData> Floors     # 다층 구조
├── float TileSize = 400          # 타일 간격 (cm)
├── float FloorHeight = 300       # 층 높이 (cm)
└── float BuildingScale = 1.0     # 균일 스케일 (0.1~10.0)
```

### 스케일 시스템

메쉬 임포트 크기에 관계없이 자동 보정:

```
CachedMeshScale = BASE_TILE_SIZE(400) / 참조메쉬.MaxDim

위치 = (TileToWorld(층, X, Y) + Pivot(TileSize/2)) × BuildingScale
스케일 = CachedMeshScale × (TileSize/400) × BuildingScale    ← XY
         CachedMeshScale × (FloorHeight/300) × BuildingScale  ← Z
```

| 임포트 상태 | MaxDim | CachedMeshScale | 결과 |
|:---|:---:|:---:|:---|
| 정상 (400cm) | 400 | 1.0 | 그대로 |
| 100배 과대 (40000cm) | 40000 | 0.01 | 자동 축소 |
| 단위 미변환 (4cm) | 4 | 100.0 | 자동 확대 |

### Building Editor (에디터 윈도우)

#### 기능 목록
| 기능 | 설명 |
|:---|:---|
| **타일 페인트** | 12종 타일 팔레트에서 선택 후 클릭으로 배치 |
| **Zoom/Pan** | 마우스휠 줌, 중간버튼 드래그 팬 |
| **층 관리** | Add Floor (현재 층 복사), Copy, Paste, Delete |
| **Rebuild HISM** | 타일 데이터 → 3D 메쉬 인스턴스 재생성 |
| **뷰포트 연동** | 타일 클릭 시 3D 뷰포트 카메라 해당 위치로 포커스 |
| **Point Light 배치** | 팔레트에서 Point Light 선택 후 클릭으로 조명 설치 |
| **Undo/Redo** | 모든 편집 작업 FScopedTransaction 지원 |

#### 도구 모드
| 모드 | 동작 |
|:---|:---|
| Paint | 타일 타입 페인트 (기본) |
| PlacePointLight | 클릭 위치에 PointLight 액터 스폰 |

### UBuildingDataAsset (메쉬 에셋)

메쉬 종류별 슬롯 매핑:
- `TMap<EWallVariant, FMeshSlot> WallMeshes` — 벽 변형별 메쉬
- `FMeshSlot DoorMesh, WindowMesh` — 문/창문
- `TMap<ETileType, FMeshSlot> FloorMeshes` — 바닥 타입별
- `FMeshSlot StairsUpMesh, StairsDownMesh` — 계단
- `FMeshSlot CeilingMesh` — 천장
- `FMeshSlot FallbackMesh` — 대체 메쉬

**AutoPopulateFromDirectory**: 지정 폴더의 FBX 메쉬를 이름 규칙으로 자동 매핑.

### 프리셋

| 프리셋 | 구성 |
|:---|:---|
| 1층 건물 | 10×8 그리드, 방 2개(A/B), 문 1개, 창문 2개 |
| 2층 건물 | 10×8 × 2층, 방 2개/층, 계단 1개, 2F 계단 개구부 |

### HISM 렌더링 파이프라인

```
RebuildHISM()
  ├── EnsureTilesSize()          # 배열 정합성 검증
  ├── ProcessBuilding()          # AutoTile 처리
  ├── ComputeCachedMeshScale()   # 참조 메쉬 기반 스케일 계산
  └── For each Floor/Tile:
      ├── AddTileInstance()      # 메인 메쉬 (벽/바닥/계단)
      ├── AddBaseFloorInstance() # 벽/계단 하부 바닥 슬랩
      └── AddCeilingInstance()   # 천장 (계단 제외)
```

### Blender 메쉬 생성

`files/create_building_meshes.py` — Blender Python 스크립트
- 기준: TILE=4.0m, WH=3.0m (벽 높이), WT=0.20m (벽 두께)
- FBX 내보내기: `global_scale=1.0`, `axis_forward='-Y'`, `axis_up='Z'`
- 생성 메쉬: Floor, Wall(6변형), Door, Window, Stairs, Ceiling

---

## 파일 경로 요약

| 파일 | 역할 |
|:---|:---|
| `BuildingTypes.h` | ETileType, EWallVariant, FTileData, FFloorData, FBuildingData |
| `BuildingActor.h/cpp` | ABuildingActor — HISM 관리, 프리셋, 인스턴스 배치 |
| `BuildingDataAsset.h/cpp` | UBuildingDataAsset — 메쉬 슬롯, AutoPopulate |
| `BuildingAutoTile.h/cpp` | 4-bit 마스크 → 벽 변형/회전 룩업 |
| `BuildingModuleActor.h/cpp` | 개별 타일 EditProxy 액터 |
| `SBuildingEditorWindow.h/cpp` | 에디터 메인 윈도우 (Slate) |
| `STilemapGrid.h/cpp` | 2D 타일맵 그리드 위젯 |
| `BuildingActorCustomization.h/cpp` | Details 패널 커스터마이즈 |
| `create_building_meshes.py` | Blender 메쉬 생성 스크립트 |
