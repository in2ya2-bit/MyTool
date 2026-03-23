# 평면도 기반 블록박스 자동 생성 — 기획문서 v1.1

> **관련 문서**: `LevelTool_StructureGenerator_기획문서.md` v1.1  
> **버전**: v1.1  
> **목적**: 건축 평면도(DXF/이미지) 또는 **파라미터 설정**을 입력받아 건물 내외부 + 교량의 블록박스(화이트박스) 메쉬를 자동 생성하여, 테스트 가능한 레벨 프로토타이핑을 지원

---

## 목차

1. [기획의도 및 범위](#1-기획의도-및-범위)
2. [전체 파이프라인](#2-전체-파이프라인)
3. [입력 데이터 포맷 — floorplan.json](#3-입력-데이터-포맷--floorplanjson)
   - 3.1 스키마 정의 | 3.2 스키마 필드 상세 | 3.3 좌표계 규칙 | 3.4 샘플 파일
4. [Track A: 평면도 → floorplan.json 변환](#4-track-a-평면도--floorplanjson-변환)
   - 4.1 DXF/SVG 벡터 파싱 | 4.2 이미지 기반 벽체 검출 | 4.3 후처리 공통 단계 | 4.4 내부 헬퍼 함수 상세 | 4.5 비직교/곡선벽 처리
5. [Track C: 파라미터 기반 floorplan 생성](#5-track-c-파라미터-기반-floorplan-생성-도면-불필요)
   - 5.1 입력 파라미터 | 5.2 생성 알고리즘 | 5.3 복도 유형별 레이아웃 | 5.4 실행 명령 | 5.5 파이프라인 통합
6. [Track B: floorplan.json → 블록박스 생성](#6-track-b-floorplanjson--블록박스-생성)
   - 6.1 건물 블록박스 | 6.2 교량 블록박스
7. [Blender 스크립트 확장 설계](#7-blender-스크립트-확장-설계)
8. [UE5 네이티브 대안 검토](#8-ue5-네이티브-대안-검토)
9. [기존 시스템 통합](#9-기존-시스템-통합)
10. [에러 핸들링 및 검증](#10-에러-핸들링-및-검증)
11. [성능 예산](#11-성능-예산)
12. [다층 건물 정합 규칙](#12-다층-건물-정합-규칙)
13. [구현 순서 및 일정](#13-구현-순서-및-일정)
14. [기획 확정 사항](#14-기획-확정-사항)

---

## 1. 기획의도 및 범위

### 배경

`StructureGenerator_기획문서.md`는 건물 유형별 **프로시저럴 룰(Treemap + BSP)** 로 실내를 생성한다.  
이 방식은 범용적이지만, **실제 건축물의 복잡한 평면 구조**(비대칭 벽체 배치, 불규칙 실 형태, 구조적 기둥 패턴)를 재현하기 어렵다.

실제 건축 평면도를 입력으로 활용하면:

| 장점 | 설명 |
|------|------|
| **현실 기반 공간 비율** | 실제 건물의 방 크기·복도 폭이 반영되어 플레이 테스트 시 스케일 감각이 정확 |
| **비정형 레이아웃 자연 생성** | L자·T자·중정(Courtyard) 구조가 데이터에 이미 포함 |
| **개구부 위치 정확** | 문·창문 위치가 평면도에 명시되어 사선·동선이 자동 반영 |
| **교량 포함** | 교량 도면도 동일 파이프라인으로 블록박스화 가능 |

### 하는 것 / 하지 않는 것 / 전제 조건

| 하는 것 | 하지 않는 것 | 전제 조건 |
|---------|------------|----------|
| DXF/SVG 평면도에서 벽·문·창 추출 | 구조 계산·하중 분석 | Python 3.10+, Blender 3.6+ |
| 이미지 평면도(PNG/JPG)에서 벽체 검출 | 고정밀 BIM 모델 생성 | OpenCV 4.x, ezdxf |
| 층별 블록박스(벽·바닥·천장·개구부) FBX 생성 | 최종 비주얼 메쉬·머티리얼 | `floorplan.json` 스키마 확정 |
| 교량 블록박스(상판·교각·난간) FBX 생성 | 케이블·행어 같은 세부 구조재 | UE5 FBX Import 파이프라인 존재 |
| `floorplan.json` 표준 포맷 정의 | 실시간 에디터 내 편집 (이 문서 범위 외) | |

### StructureGenerator와의 관계

```
[Track A] 평면도 기반 (본 문서)
  평면도 DXF/Image → floorplan.json → Blender Block-box → FBX → UE5

[Track B] 프로시저럴 기반 (StructureGenerator 기획문서)
  Structure Template + Footprint → Treemap/BSP → Whitebox Modules → UE5

[공통 출력]
  → UE5 레벨에서 게임플레이 테스트 가능한 블록박스 건물/교량
```

Track A의 `floorplan.json`은 Track B의 `structure_layout.json`과 **상호 변환 가능**하도록 설계한다. Track A로 생성한 결과물을 Track B의 에디터 편집 워크플로우에서 추가 수정 가능.

---

## 2. 전체 파이프라인

```
                          ┌──────────────────────┐
                          │   입력 (택 1)          │
                          │  ┌─ DXF/SVG 파일      │
                          │  └─ 평면도 이미지       │
                          └──────────┬───────────┘
                                     │
                    ┌────────────────┴────────────────┐
                    ▼                                  ▼
          ┌─────────────────┐               ┌──────────────────┐
          │  벡터 파싱 (4.1) │               │ 이미지 검출 (4.2) │
          │  ezdxf / svgpath │               │ OpenCV + 후처리   │
          └────────┬────────┘               └────────┬─────────┘
                   │                                  │
                   └──────────┬───────────────────────┘
                              ▼
                   ┌───────────────────┐
                   │  후처리 (4.3)      │
                   │  벽 병합 / 방 인식  │
                   │  개구부 분류        │
                   └────────┬──────────┘
                            ▼
                   ┌───────────────────┐
                   │  floorplan.json   │  ← 표준 중간 포맷
                   └────────┬──────────┘
                            │
              ┌─────────────┴─────────────┐
              ▼                            ▼
    ┌──────────────────┐        ┌──────────────────┐
    │ 건물 블록박스 (5.1)│        │ 교량 블록박스 (5.2)│
    │ Blender Script   │        │ Blender Script   │
    └────────┬─────────┘        └────────┬─────────┘
             │                            │
             ▼                            ▼
    ┌──────────────────────────────────────┐
    │             FBX 출력                  │
    │  per_floor / combined / bridge       │
    └──────────────────┬───────────────────┘
                       ▼
              ┌─────────────────┐
              │  UE5 Import     │
              │  레벨 배치       │
              └─────────────────┘
```

---

## 3. 입력 데이터 포맷 — floorplan.json

모든 파싱 결과와 블록박스 생성의 **단일 교환 포맷**.

### 3.1 스키마 정의

```jsonc
{
  // ── 메타 ──
  "version": "1.0",
  "building_name": "string",              // 식별 이름
  "source": "dxf | svg | image | manual", // 원본 데이터 유형
  "source_file": "path/to/original",      // 원본 파일 경로 (참조용)

  // ── 스케일 ──
  "scale": {
    "unit": "meter",          // 좌표 단위 (항상 meter로 정규화)
    "pixels_per_meter": 50,   // image 소스일 때만 유효
    "origin": [0.0, 0.0]     // 좌측 하단 기준점
  },

  // ── 층 배열 ──
  "floors": [
    {
      "floor_index": 0,        // 0 = 1층 (지상), -1 = 지하1층
      "label": "1F",
      "elevation_m": 0.0,      // 이 층 바닥의 절대 높이
      "height_m": 3.2,         // 이 층의 천장 높이

      // ── 벽체 ──
      "walls": [
        {
          "wall_id": "w_001",
          "start": [0.0, 0.0],       // [x, y] meter
          "end": [12.0, 0.0],
          "thickness_m": 0.2,
          "type": "exterior | interior | partition",
          "openings": [
            {
              "opening_id": "op_001",
              "type": "door | window | shutter | archway",
              "offset_m": 3.0,       // start 기준 벽체 방향 오프셋
              "width_m": 1.2,
              "height_m": 2.1,
              "sill_m": 0.0          // 바닥 기준 하단 높이 (door=0, window=보통 0.9)
            }
          ]
        }
      ],

      // ── 방(공간) ──
      "rooms": [
        {
          "room_id": "r_001",
          "label": "로비",
          "boundary_wall_ids": ["w_001", "w_002", "w_003", "w_004"],
          "polygon": [[0,0], [12,0], [12,8], [0,8]],  // 닫힌 폴리곤 (미터)
          "area_m2": 96.0,
          "usage": "lobby | classroom | office | corridor | stairwell | utility | production | storage | restroom | other"
        }
      ],

      // ── 계단 ──
      "stairs": [
        {
          "stair_id": "st_001",
          "location": [10.0, 6.0],   // 중심점
          "size_m": [3.0, 5.0],      // [폭, 깊이]
          "type": "straight | 180_turn | spiral | L_turn",
          "connects_floors": [0, 1], // 연결 층 인덱스
          "direction_deg": 0         // 오르는 방향 (0=+Y)
        }
      ],

      // ── 기둥 ──
      "columns": [
        {
          "column_id": "col_001",
          "center": [6.0, 4.0],
          "size_m": [0.4, 0.4],      // [가로, 세로] 또는 원형이면 [직경]
          "shape": "square | circle"
        }
      ],

      // ── 엘리베이터 ──
      "elevators": [
        {
          "elevator_id": "ev_001",
          "center": [11.0, 7.0],
          "size_m": [2.0, 2.0],
          "connects_floors": [0, 1, 2]
        }
      ]
    }
  ],

  // ── 교량 (독립 섹션) ──
  "bridges": [
    {
      "bridge_id": "br_001",
      "label": "Main Bridge",
      "type": "beam | arch | truss | suspension | cable_stayed",

      "path": [              // 중심선 3D 경로점 (meter)
        [0.0, 0.0, 10.0],
        [50.0, 0.0, 10.0],
        [100.0, 0.0, 8.0]
      ],

      "deck": {
        "width_m": 12.0,
        "thickness_m": 1.0,
        "lanes": 2,
        "sidewalk_width_m": 1.5,
        "sidewalk_height_offset_m": 0.15
      },

      "piers": [
        {
          "pier_id": "p_001",
          "station_m": 50.0,     // path 시작점으로부터의 거리
          "width_m": 3.0,
          "depth_m": 2.0,
          "top_z": 10.0,
          "bottom_z": 0.0
        }
      ],

      "abutments": [
        {
          "abutment_id": "ab_001",
          "station_m": 0.0,
          "width_m": 14.0,
          "depth_m": 3.0,
          "top_z": 10.0,
          "bottom_z": 5.0
        }
      ],

      "railings": {
        "height_m": 1.1,
        "thickness_m": 0.15,
        "post_spacing_m": 2.0
      }
    }
  ],

  // ── 메타데이터 ──
  "metadata": {
    "author": "string",
    "created": "ISO-8601",
    "notes": "string"
  }
}
```

### 3.2 스키마 필드 상세

#### 벽체 (wall)

| 필드 | 타입 | 필수 | 설명 |
|------|------|------|------|
| `wall_id` | string | Y | 고유 식별자. `w_` 접두사 권장 |
| `start`, `end` | [float, float] | Y | 벽체 중심선의 시작/끝점 (meter) |
| `thickness_m` | float | Y | 벽체 두께. 외벽 0.2~0.3, 내벽 0.1~0.15 |
| `type` | enum | Y | `exterior`: 건물 외벽, `interior`: 내부 구조벽, `partition`: 경량 칸막이 |
| `openings` | array | N | 이 벽에 포함된 문·창문 |

#### 개구부 (opening)

| 필드 | 타입 | 필수 | 설명 |
|------|------|------|------|
| `opening_id` | string | Y | 고유 식별자. `op_` 접두사 |
| `type` | enum | Y | `door`, `window`, `shutter`, `archway` |
| `offset_m` | float | Y | `start`로부터 벽체 방향으로의 거리 |
| `width_m` | float | Y | 개구부 폭 |
| `height_m` | float | Y | 개구부 높이 |
| `sill_m` | float | N | 바닥 기준 하단 높이. 기본값: door=0, window=0.9, shutter=0 |

#### 방 (room)

| 필드 | 타입 | 필수 | 설명 |
|------|------|------|------|
| `room_id` | string | Y | 고유 식별자 |
| `label` | string | N | 표시 이름 (로비, 교실 A 등) |
| `boundary_wall_ids` | [string] | N | 이 방에 인접한 벽체 ID 배열. **참조용** — 부분 인접도 포함 가능 |
| `polygon` | [[float,float]] | Y | 방 경계 닫힌 폴리곤 (**기하학적 Truth**). 벽 중심선 기준. 반시계(CCW) 방향. **마지막 점은 첫 점과 다름** — 암묵적 닫힘 (3점이면 삼각형) |
| `area_m2` | float | N | 산출 면적 (자동 계산 가능. polygon으로부터 Shoelace 공식) |
| `usage` | enum | N | 공간 용도 분류 |

> **v1.1 변경**: `boundary_wall_ids`를 필수→선택으로 변경. 벽이 방 경계를 부분적으로만 접하는 경우(예: 중복도형 학교에서 긴 외벽이 복도와 교실 모두에 접함)를 허용. **`polygon`이 방 경계의 유일한 기하학적 Truth**이며, `boundary_wall_ids`는 인접 관계 참조용.

#### 계단 (stairs)

| 필드 | 타입 | 필수 | 설명 |
|------|------|------|------|
| `stair_id` | string | Y | 고유 식별자 |
| `location` | [float, float] | Y | 계단 바운딩박스 중심점 |
| `size_m` | [float, float] | Y | [폭, 깊이] |
| `type` | enum | Y | `straight`, `180_turn`, `spiral`, `L_turn` |
| `connects_floors` | [int] | Y | 연결하는 층 인덱스 배열 |
| `direction_deg` | float | N | 오르는 방향. 기본 0 (+Y) |

#### 교량 데크 (deck)

| 필드 | 타입 | 필수 | 설명 |
|------|------|------|------|
| `width_m` | float | Y | 전체 폭 (차도 + 인도 포함) |
| `thickness_m` | float | Y | 상판 두께 |
| `lanes` | int | N | 차선 수. 블록박스에서는 시각적 참조용 |
| `sidewalk_width_m` | float | N | 인도 폭. 0이면 인도 없음 |
| `sidewalk_height_offset_m` | float | N | 인도 높이차 (데크 상면 기준) |

#### 교각 (pier) / 교대 (abutment)

| 필드 | 타입 | 필수 | 설명 |
|------|------|------|------|
| `station_m` | float | Y | path 시작점으로부터의 누적 거리 |
| `width_m` | float | Y | 교각/교대 폭 |
| `depth_m` | float | Y | 교각/교대 깊이 (도로 방향) |
| `top_z`, `bottom_z` | float | Y | 상·하단 절대 높이 |

### 3.3 좌표계 규칙

모든 좌표 데이터의 해석 방법.

| 항목 | 규칙 |
|------|------|
| **floorplan.json 내부** | 2D 좌표 `[x, y]` = 미터 단위, 좌측 하단 원점, X=오른쪽, Y=위쪽 |
| **교량 path** | 3D 좌표 `[x, y, z]` = 미터 단위, Z=높이 (Blender Z-up 일치) |
| **Blender 생성 시** | floorplan.json 좌표를 그대로 사용 (미터 단위, Z-up) |
| **FBX 내보내기** | `export_fbx_ue5()` 설정으로 UE5 좌표계 자동 변환 (Forward=-Z, Up=Y) |
| **UE5 Import 후** | 1 unit = 1 cm. Blender 1m = UE5 100cm (`apply_unit_scale=True`) |

**미터 → cm 변환 시점**: Blender FBX 내보내기 단계에서 자동 변환. `floorplan.json`과 Blender 스크립트 내부에서는 항상 **미터** 사용.

**원점 기준**:
- 건물: `scale.origin`이 건물 좌측 하단. 모든 벽체/방 좌표는 이 원점 기준 상대값.
- 교량: `path[0]`이 교량 시작점. UE5에서의 월드 위치는 Import 후 수동 배치 또는 별도 배치 스크립트 사용.

### 3.4 샘플 파일

| 파일 | 설명 | 주요 특성 |
|------|------|----------|
| `files/samples/floorplan_factory_sample.json` | 공장 2층 (60m×40m) | 1F: 생산라인 1200m² + 창고 600m² + 사무실, 2F: 관리동. 기둥 6개, 셔터 문 |
| `files/samples/floorplan_school_sample.json` | 학교 3층 (50m×15m) | 중복도형. 교실×8 + 강당 + 화장실. 양쪽 계단실 |
| `files/samples/floorplan_bridge_sample.json` | 교량 2종 | 200m 4차선 빔 교량 + 45m 보행자 육교 |

---

## 4. Track A: 평면도 → floorplan.json 변환

### 4.1 DXF/SVG 벡터 파싱

DXF(AutoCAD) 또는 SVG 파일에서 벽체 라인을 직접 추출. 벡터 데이터는 정밀도가 높아 벽체 방향·길이·두께를 정확히 얻을 수 있다.

**DXF 파싱 흐름**:

```
DXF 파일
  ├── Layer 분류 (WALL, DOOR, WINDOW, STAIR, COLUMN ...)
  ├── Entity 추출 (LINE, LWPOLYLINE, ARC, CIRCLE, INSERT)
  │    ├── LINE → 벽체 중심선 후보
  │    ├── LWPOLYLINE → 닫힌 폴리곤 → 방 경계 또는 벽체
  │    ├── ARC → 곡선벽 세그먼트 (직선 근사)
  │    ├── CIRCLE → 원형 기둥 후보
  │    └── INSERT (Block Reference) → 문/창 심볼 위치
  └── 좌표 정규화 (DXF 단위 → meter)
```

**주요 처리 로직**:

```python
import ezdxf
from collections import defaultdict

def parse_dxf_floorplan(dxf_path: str, floor_index: int = 0,
                         floor_height: float = 3.2) -> dict:
    doc = ezdxf.readfile(dxf_path)
    msp = doc.modelspace()

    walls = []
    openings_by_wall = defaultdict(list)
    rooms = []
    columns = []
    wall_counter = 0

    for entity in msp:
        layer = entity.dxf.layer.upper()

        if "WALL" in layer:
            if entity.dxftype() == "LINE":
                wall_counter += 1
                walls.append({
                    "wall_id": f"w_{wall_counter:03d}",
                    "start": [entity.dxf.start.x, entity.dxf.start.y],
                    "end": [entity.dxf.end.x, entity.dxf.end.y],
                    "thickness_m": _infer_thickness(layer),
                    "type": "exterior" if "EXT" in layer else "interior",
                    "openings": []
                })
            elif entity.dxftype() == "LWPOLYLINE":
                points = list(entity.get_points(format="xy"))
                for i in range(len(points)):
                    j = (i + 1) % len(points)
                    wall_counter += 1
                    walls.append({
                        "wall_id": f"w_{wall_counter:03d}",
                        "start": list(points[i]),
                        "end": list(points[j]),
                        "thickness_m": _infer_thickness(layer),
                        "type": "exterior" if "EXT" in layer else "interior",
                        "openings": []
                    })

        elif "DOOR" in layer or "WINDOW" in layer:
            if entity.dxftype() == "INSERT":
                _register_opening(entity, walls, openings_by_wall)

        elif "COLUMN" in layer:
            if entity.dxftype() == "CIRCLE":
                columns.append({
                    "column_id": f"col_{len(columns)+1:03d}",
                    "center": [entity.dxf.center.x, entity.dxf.center.y],
                    "size_m": [entity.dxf.radius * 2],
                    "shape": "circle"
                })

    _attach_openings_to_walls(walls, openings_by_wall)

    return {
        "floor_index": floor_index,
        "label": f"{floor_index+1}F",
        "elevation_m": floor_index * floor_height,
        "height_m": floor_height,
        "walls": walls,
        "rooms": _detect_rooms_from_walls(walls),
        "stairs": [],
        "columns": columns,
        "elevators": []
    }
```

**DXF 레이어 이름 규약 (권장)**:

| 레이어 패턴 | 매핑 |
|-------------|------|
| `*WALL_EXT*` | 외벽 |
| `*WALL_INT*` | 내벽 |
| `*WALL_PART*` | 경량 칸막이 |
| `*DOOR*` | 문 INSERT 블록 |
| `*WINDOW*`, `*WIN*` | 창문 INSERT 블록 |
| `*STAIR*` | 계단 영역 |
| `*COLUMN*`, `*COL*` | 기둥 |
| `*ELEV*` | 엘리베이터 |

> DXF 레이어 이름이 규약과 다를 경우, 파싱 전 레이어 매핑 설정 파일(`layer_mapping.json`)을 제공하여 커스터마이즈.

**SVG 파싱**: DXF와 동일한 출력 구조. `svgpathtools` 라이브러리로 `<path>`, `<line>`, `<rect>` 요소를 추출한 후 동일 후처리 적용.

### 4.2 이미지 기반 벽체 검출

스캔된 평면도 이미지(PNG/JPG)에서 벽체를 검출. 벡터 대비 정밀도가 낮으므로 후처리 보정이 핵심.

**검출 파이프라인**:

```
원본 이미지
  │
  ├── 1. 전처리
  │    ├── Grayscale 변환
  │    ├── Gaussian Blur (σ=1.5, 노이즈 제거)
  │    └── Adaptive Threshold (blockSize=11, C=2)
  │
  ├── 2. 벽체 추출
  │    ├── Morphology Close (kernel 3×3, iter=2) → 텍스트/해칭 제거
  │    ├── Morphology Open (kernel 2×2, iter=1) → 미세 노이즈 제거
  │    ├── Thinning (Zhang-Suen) → 벽체 중심선 1px
  │    └── HoughLinesP (threshold=50, minLineLength=30, maxLineGap=10)
  │
  ├── 3. 라인 후처리
  │    ├── 근접 라인 병합 (각도 차 < 5°, 거리 < 10px → 단일 라인)
  │    ├── 직교 스냅 (각도가 수평/수직 ±3° 이내 → 정확히 0°/90°로 보정)
  │    └── Gap 검출 (연속 라인 사이 간격 → 문/창 후보)
  │
  ├── 4. 개구부 분류
  │    ├── Gap 폭 0.7~1.5m → door 후보
  │    ├── Gap 폭 0.5~3.0m + 벽 중간 → window 후보
  │    └── Arc 검출 (문 개폐 호) → door 확정
  │
  └── 5. 좌표 변환
       └── pixel → meter (pixels_per_meter 기준)
```

**핵심 알고리즘 상세**:

```python
import cv2
import numpy as np

def detect_walls_from_image(image_path: str,
                             pixels_per_meter: float = 50.0,
                             wall_thickness_px: int = 3) -> dict:
    img = cv2.imread(image_path, cv2.IMREAD_GRAYSCALE)
    h, w = img.shape

    # 1. 전처리
    blurred = cv2.GaussianBlur(img, (5, 5), 1.5)
    binary = cv2.adaptiveThreshold(
        blurred, 255, cv2.ADAPTIVE_THRESH_GAUSSIAN_C,
        cv2.THRESH_BINARY_INV, 11, 2
    )

    # 2. Morphology — 텍스트/심볼 제거, 벽체 강조
    kernel_close = cv2.getStructuringElement(cv2.MORPH_RECT, (3, 3))
    closed = cv2.morphologyEx(binary, cv2.MORPH_CLOSE, kernel_close, iterations=2)

    kernel_open = cv2.getStructuringElement(cv2.MORPH_RECT, (2, 2))
    cleaned = cv2.morphologyEx(closed, cv2.MORPH_OPEN, kernel_open, iterations=1)

    # 3. Thinning → 중심선
    thinned = cv2.ximgproc.thinning(cleaned) if hasattr(cv2, 'ximgproc') else cleaned

    # 4. Hough Line 검출
    lines = cv2.HoughLinesP(
        thinned, rho=1, theta=np.pi/180,
        threshold=50, minLineLength=30, maxLineGap=10
    )

    if lines is None:
        return {"walls": [], "gaps": []}

    # 5. 라인 병합 + 직교 스냅
    raw_segments = [(l[0][0], l[0][1], l[0][2], l[0][3]) for l in lines]
    merged = _merge_collinear_segments(raw_segments, angle_tol=5.0, dist_tol=10)
    snapped = _snap_to_orthogonal(merged, angle_tol=3.0)

    # 6. pixel → meter 변환
    walls = []
    for i, (x1, y1, x2, y2) in enumerate(snapped):
        walls.append({
            "wall_id": f"w_{i+1:03d}",
            "start": [x1 / pixels_per_meter, (h - y1) / pixels_per_meter],
            "end": [x2 / pixels_per_meter, (h - y2) / pixels_per_meter],
            "thickness_m": wall_thickness_px / pixels_per_meter,
            "type": "exterior",  # 이후 방 인식으로 interior/exterior 재분류
            "openings": []
        })

    # 7. Gap 검출 → 개구부 후보
    gaps = _detect_gaps(snapped, max_gap_px=int(3.0 * pixels_per_meter))
    _classify_and_attach_openings(walls, gaps, pixels_per_meter)

    return {"walls": walls, "gaps": gaps}
```

**정확도 목표 및 한계**:

| 항목 | 목표 | 비고 |
|------|------|------|
| 벽체 검출률 | ≥ 85% | 직선 벽 기준. 곡선벽은 직선 근사 |
| 개구부 검출률 | ≥ 70% | Arc 심볼 있는 문이 가장 정확 |
| 위치 오차 | ≤ 0.3m | pixel 해상도 의존. 300DPI 이상 권장 |
| 벽 유형 분류 | ≥ 80% | exterior/interior는 방 인식 후 재분류 |

### 4.3 후처리 공통 단계

DXF 파싱과 이미지 검출 모두 동일한 후처리를 거쳐 `floorplan.json`을 생성.

```
Raw walls + openings
  │
  ├── 1. 벽 교차점 정리
  │    ├── T-junction 인식 → 벽 분할
  │    └── 교차 벽 끝점 스냅 (오차 < 0.1m → 동일 점으로 병합)
  │
  ├── 2. 방(Room) 인식
  │    ├── 벽 그래프 구성 (노드: 교차점, 엣지: 벽 세그먼트)
  │    ├── 최소 순환(Minimum Cycle) 탐색 → 닫힌 폴리곤 = 방
  │    ├── 면적 필터: 1m² 미만 → 기둥/설비로 분류, 500m² 초과 → 외부 영역 제외
  │    └── 방 라벨 자동 추정 (면적 + 문 수 + 인접 관계 기반)
  │
  ├── 3. 벽 유형 재분류
  │    ├── 한쪽만 방이 인접 → exterior
  │    ├── 양쪽에 방이 인접 → interior
  │    └── thickness > 0.2m → exterior 후보 가중치 증가
  │
  ├── 4. 계단/엘리베이터 인식
  │    ├── DXF: 전용 레이어에서 추출
  │    └── Image: 평행선 패턴 (계단 디딤면) 검출 or 수동 지정
  │
  └── 5. floorplan.json 출력
       ├── 검증: 닫히지 않은 방 경고, 고립 벽 경고
       └── 통계 출력: 벽 N개, 방 N개, 개구부 N개
```

**방 라벨 자동 추정 규칙**:

| 조건 | 추정 라벨 |
|------|----------|
| 면적 < 4m² + 문 1개 | `restroom` 또는 `utility` |
| 면적 4~15m² + 문 1개 | `office` 또는 `classroom` |
| 면적 > 50m² + 문 ≥ 2개 | `lobby` 또는 `production` |
| 폭 < 3m + 길이/폭 > 3.0 | `corridor` |
| 계단 포함 | `stairwell` |

### 4.4 내부 헬퍼 함수 상세

문서 내 의사코드에서 호출하는 내부 함수의 입출력 계약.

| 함수 | 입력 | 출력 | 핵심 로직 |
|------|------|------|----------|
| `_infer_thickness(layer_name)` | DXF 레이어 이름 `str` | `float` (미터) | `EXT` 포함→0.25, `INT`→0.15, `PART`→0.12, 기본→0.15 |
| `_register_opening(entity, walls, openings_map)` | INSERT entity, 벽 리스트, dict | `None` (openings_map에 추가) | INSERT의 insertion_point에서 가장 가까운 벽 탐색 → 블록 이름으로 door/window 분류 → offset_m 산출 (벽 start 기준 투영) |
| `_attach_openings_to_walls(walls, openings_map)` | 벽 리스트, {wall_idx: [openings]} | `None` (walls 수정) | openings_map의 각 opening을 해당 벽의 `openings` 배열에 추가. offset 순 정렬 |
| `_detect_rooms_from_walls(walls)` | 벽 리스트 | `[room dict]` | `detect_rooms()`와 동일. 벽 그래프 → 최소순환 → polygon + area 산출 |
| `_merge_collinear_segments(segments, angle_tol, dist_tol)` | `[(x1,y1,x2,y2)]`, 각도 허용치(°), 거리 허용치(px) | 병합된 `[(x1,y1,x2,y2)]` | 각도 차 < angle_tol 이고 수직 거리 < dist_tol인 세그먼트 쌍 → 시작/끝점을 양 극단으로 확장하여 단일 세그먼트로 병합 |
| `_snap_to_orthogonal(segments, angle_tol)` | `[(x1,y1,x2,y2)]`, 각도 허용치(°) | 보정된 `[(x1,y1,x2,y2)]` | 수평(0°/180°) 또는 수직(90°/270°)에서 angle_tol 이내인 세그먼트를 정확히 수평/수직으로 보정. 나머지는 원본 유지 |
| `_detect_gaps(segments, max_gap_px)` | `[(x1,y1,x2,y2)]`, 최대 갭 크기(px) | `[{wall_idx, offset_px, width_px}]` | 동일선 세그먼트 간 갭(빈 공간) 검출. 갭 크기 ≤ max_gap_px인 것만 반환 |
| `_classify_and_attach_openings(walls, gaps, ppm)` | 벽 리스트, 갭 리스트, pixels_per_meter | `None` (walls 수정) | 갭 폭 0.7~1.5m→door, 0.5~3.0m→window 분류. offset_m = gap_offset_px / ppm. 해당 벽의 openings에 추가 |

### 4.5 비직교 / 곡선벽 처리 전략

대부분의 건축 평면도는 직교(0°/90°) 벽이 주를 이루지만, 비직교 벽도 존재한다.

**비직교 벽 (대각선)**:

| 상황 | 처리 |
|------|------|
| 이미지 검출: 각도가 수평/수직 ±3° 밖 | 직교 스냅 **하지 않음**. 원본 각도 유지 |
| Blender 벽체 생성 | `create_wall_block()`의 `angle` 파라미터가 임의 각도 지원. 추가 처리 불필요 |
| Boolean 개구부 | 커터 박스도 동일 `angle`로 회전하므로 정상 동작 |
| 방 인식 | 최소순환 알고리즘은 각도에 무관. 비직교 벽도 그래프 엣지로 처리 |

**곡선벽 (ARC)**:

| 단계 | 처리 | 파라미터 |
|------|------|---------|
| DXF ARC 추출 | `entity.dxf.start_angle`, `end_angle`, `radius` 획득 | — |
| 직선 근사 | 호를 N개 직선 세그먼트로 분할 | N = max(4, arc_length / 1.0m). 1m당 최소 1세그먼트 |
| 벽 생성 | 각 세그먼트를 독립 벽으로 처리 | 세그먼트별 wall_id 자동 부여 |
| 개구부 | 곡선벽 위의 개구부는 가장 가까운 세그먼트에 배치 | offset을 호 길이 기준→세그먼트 로컬 offset으로 변환 |

**이미지 곡선벽**: HoughLinesP는 직선만 검출. 곡선은 짧은 직선 세그먼트 여러 개로 자동 분해됨. `_merge_collinear_segments`에서 각도 차가 큰 경우 병합하지 않으므로 곡선 형태가 보존.

---

## 5. Track C: 파라미터 기반 floorplan 생성 (도면 불필요)

도면 없이 **설정값만으로** `floorplan.json`을 프로시저럴 생성. 빠른 게임플레이 프로토타이핑 용도.

### 5.1 입력 파라미터

#### 필수 파라미터

| 파라미터 | 타입 | 범위 | 설명 |
|---------|------|------|------|
| `building_width` | float | 5~200m | 건물 X축 전체 폭 |
| `building_depth` | float | 5~200m | 건물 Y축 전체 깊이 |
| `num_floors` | int | 1~10 | 층수 |
| `rooms_per_floor` | int | 1~30 | 층당 방 수 (복도 제외) |
| `has_windows` | bool | — | 외벽에 창문 자동 배치 여부 |
| `door_placement` | enum | `every_room` / `corridor_only` / `manual` | 문 배치 전략 |

#### 선택 파라미터 (합리적 기본값 제공)

| 파라미터 | 타입 | 기본값 | 설명 |
|---------|------|--------|------|
| `floor_height` | float | 3.5m | 층고 |
| `corridor_type` | enum | `single` | `none` / `single` / `double` / `L` / `loop` |
| `corridor_width` | float | 3.0m | 복도 폭 |
| `building_shape` | enum | `rectangle` | `rectangle` / `L` / `T` / `U` |
| `exterior_wall_thickness` | float | 0.25m | 외벽 두께 |
| `interior_wall_thickness` | float | 0.15m | 내벽 두께 |
| `window_interval` | float | 3.0m | 외벽 창문 간격 (has_windows=true 시) |
| `window_width` | float | 1.5m | 창문 폭 |
| `window_height` | float | 1.5m | 창문 높이 |
| `window_sill` | float | 0.9m | 창문 하단 높이 |
| `door_width` | float | 1.0m | 문 폭 |
| `door_height` | float | 2.1m | 문 높이 |
| `main_entrance_width` | float | 2.0m | 정문 폭 |
| `add_stairs` | bool | true | 2층 이상 시 계단 자동 배치 |
| `stair_location` | enum | `end` | `end` / `center` / `both_ends` |
| `room_labels` | [string] | [] | 방 이름 목록 (빈 배열 → 자동 "Room_01" 등) |
| `seed` | int | 0 | 랜덤 시드 (0=시간 기반). 동일 시드 → 동일 결과 |

### 5.2 생성 알고리즘

```
입력 파라미터
  │
  ├── 1. 건물 외곽 생성
  │    ├── rectangle: 4개 외벽
  │    ├── L: 6개 외벽 (주동 + 별동 분할 비율 = 60:40)
  │    ├── T: 8개 외벽 (중앙돌출 = 전체폭 40%)
  │    └── U: 8개 외벽 (양측익 = 전체깊이 40%)
  │
  ├── 2. 복도 배치
  │    ├── none: 복도 없음 → 방이 외벽에 직접 접함
  │    ├── single: Y축 중앙 또는 한쪽에 직선 복도
  │    ├── double: 중앙 복도 + 양쪽 방 열
  │    ├── L: L자 복도 (L형 건물에 최적)
  │    └── loop: 중정형 순환 복도
  │
  ├── 3. 방 분할
  │    ├── 복도 한쪽/양쪽의 가용 영역 산출
  │    ├── 가용 영역을 rooms_per_floor 등분 (균등 폭)
  │    ├── 최소 방 크기 검증 (3m × 3m 미만 → 방 수 자동 축소 + Warning)
  │    └── 각 방에 polygon, area_m2, room_id, label 부여
  │
  ├── 4. 개구부(문/창) 배치
  │    ├── 창문: 외벽에 window_interval 간격으로 자동 배치
  │    │    └── 벽 시작점에서 1m 오프셋 → 간격마다 반복 → 벽 끝 1m 전 종료
  │    ├── 문 (every_room): 각 방 → 복도 접하는 벽의 중앙에 문 1개
  │    ├── 문 (corridor_only): 복도 양단에만 문
  │    └── 정문: 1층 외벽 (남쪽=Y최소) 중앙에 main_entrance 배치
  │
  ├── 5. 계단 배치 (num_floors ≥ 2 && add_stairs)
  │    ├── end: 복도 끝단에 계단실 1개 (방 1개 대체)
  │    ├── center: 복도 중앙에 계단실 1개
  │    └── both_ends: 양 끝단에 계단실 2개
  │
  └── 6. 층 복제
       ├── 1층 레이아웃 → 2~N층 복제
       ├── 층별 elevation_m = Σ(이전층 height_m)
       ├── 1층 전용: 정문(main_entrance)
       └── 최상층: 옥상 접근 계단 (선택)
```

### 5.3 복도 유형별 레이아웃 예시

**single 복도** (가장 일반적):
```
┌─────────────────────────────────────┐
│  Room 1  │ Room 2 │ Room 3 │ Room 4│  ← 방 열
├──────────┴────────┴────────┴───────┤
│              복  도                  │  ← corridor_width
└─────────────────────────────────────┘
```

**double 복도**:
```
┌────────┬────────┬────────┬────────┐
│ Room 1 │ Room 2 │ Room 3 │ Room 4 │  ← 북쪽 방 열
├────────┴────────┴────────┴────────┤
│              복  도                 │
├────────┬────────┬────────┬────────┤
│ Room 5 │ Room 6 │ Room 7 │ Room 8 │  ← 남쪽 방 열
└────────┴────────┴────────┴────────┘
```

**L 복도** (building_shape=L):
```
┌────────┬──────┐
│ Room 5 │ R 6  │
├────────┤      │
│ 복 도  ├──────┘
├────────┤
│ Room 3 │ Room 4 │ ← 주동
├────────┴────────┤
│     복  도       │
├────────┬────────┤
│ Room 1 │ Room 2 │
└────────┴────────┘
```

### 5.4 실행 명령

```bash
# 기본 사용: 20m×12m, 3층, 층당 4실, 창문+문 전부
python floorplan_generator.py \
  --width 20 --depth 12 --floors 3 --rooms 4 \
  --windows --doors every_room \
  --output hospital_proto.json

# L자 건물, 복도 없이 큰 방 2개
python floorplan_generator.py \
  --width 30 --depth 20 --floors 1 --rooms 2 \
  --shape L --corridor none --doors every_room \
  --output warehouse.json

# 학교형: double 복도, 양단 계단
python floorplan_generator.py \
  --width 50 --depth 15 --floors 3 --rooms 8 \
  --corridor double --stairs both_ends \
  --windows --doors every_room --seed 42 \
  --output school_proto.json
```

### 5.5 파이프라인 통합

```
파라미터 설정 ──→ floorplan_generator.py ──→ floorplan.json ──→ (기존 Track B)
                                                              ├→ blender_blockbox.py → FBX
                                                              └→ visualize_blockbox.py → 미리보기
```

기존 Track A(DXF/이미지 파싱)와 **동일한 출력 포맷**(`floorplan.json`)을 사용하므로, 이후 블록박스 생성·UE5 Import 과정이 완전히 동일.

---

## 6. Track B: floorplan.json → 블록박스 생성

### 6.1 건물 블록박스

`floorplan.json`의 각 층을 순회하며 블록박스 지오메트리를 생성.

**생성 순서**:

```
floorplan.json 로드
  │
  ├── 층별 반복 (floor_index 순)
  │    │
  │    ├── 1. 바닥판 (Floor Plate)
  │    │    └── 방 polygon 합집합 → 단일 평면 메쉬 (두께 0.15~0.2m)
  │    │
  │    ├── 2. 천장판 (Ceiling Plate)
  │    │    └── 바닥판과 동일 형태, elevation + height_m 위치
  │    │
  │    ├── 3. 벽체 (Wall Box)
  │    │    ├── 벽 중심선 → 두께 적용 → 직육면체 메쉬
  │    │    ├── 높이: floor height_m
  │    │    └── 개구부 Boolean DIFFERENCE 적용
  │    │
  │    ├── 4. 개구부 프레임 (선택)
  │    │    └── 문/창 외곽에 얇은 프레임 박스 (시각적 구분)
  │    │
  │    ├── 5. 기둥 (Column Box)
  │    │    └── square: 직육면체, circle: 8각기둥 근사
  │    │
  │    └── 6. 계단 블록
  │         ├── straight: 경사면 박스
  │         ├── 180_turn: 2개 경사면 + 중간 참 (landing)
  │         ├── L_turn: 2개 경사면 + 코너 참
  │         └── spiral: N개 세그먼트 회전 배치
  │
  └── 전체 조립 → FBX 출력
```

**벽체 메쉬 생성 상세**:

```python
def create_wall_block(wall: dict, floor_elevation: float,
                       floor_height: float) -> bpy.types.Object:
    sx, sy = wall["start"]
    ex, ey = wall["end"]
    t = wall["thickness_m"]

    length = math.sqrt((ex - sx)**2 + (ey - sy)**2)
    if length < 0.01:
        return None
    angle = math.atan2(ey - sy, ex - sx)

    cx = (sx + ex) / 2.0
    cy = (sy + ey) / 2.0
    cz = floor_elevation + floor_height / 2.0

    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(cx, cy, cz))
    obj = bpy.context.active_object
    obj.scale = (length, t, floor_height)
    obj.rotation_euler.z = angle

    # ★ Boolean 연산 전 반드시 스케일/회전을 메쉬에 적용
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)

    obj.name = f"Wall_{wall['wall_id']}"

    # 개구부 Boolean 적용
    for opening in wall.get("openings", []):
        _apply_opening_boolean(obj, opening, wall, floor_elevation)

    return obj


def _apply_opening_boolean(wall_obj: bpy.types.Object,
                            opening: dict,
                            wall: dict,
                            floor_elevation: float) -> None:
    sx, sy = wall["start"]
    ex, ey = wall["end"]
    wall_len = math.sqrt((ex-sx)**2 + (ey-sy)**2)
    angle = math.atan2(ey - sy, ex - sx)

    offset = opening["offset_m"]
    w = opening["width_m"]
    h = opening["height_m"]
    sill = opening.get("sill_m", 0.0)

    # 개구부 범위 검증 (V-5)
    if offset + w > wall_len + 0.01:
        print(f"  Warning: opening {opening.get('opening_id')} exceeds wall length, clamping")
        offset = max(0, wall_len - w)

    # 개구부 중심 위치 (벽체 시작점 기준 오프셋 → 월드 좌표)
    center_along = offset + w / 2.0
    dx, dy = (ex - sx) / wall_len, (ey - sy) / wall_len
    ox = sx + dx * center_along
    oy = sy + dy * center_along
    oz = floor_elevation + sill + h / 2.0

    # 커터 박스 생성 + 즉시 transform_apply
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(ox, oy, oz))
    cutter = bpy.context.active_object
    cutter.scale = (w, wall["thickness_m"] * 2, h)
    cutter.rotation_euler.z = angle
    bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
    cutter.name = f"Cutter_{opening.get('opening_id', 'op')}"

    # Boolean Difference
    try:
        mod = wall_obj.modifiers.new(name="Opening", type="BOOLEAN")
        mod.operation = "DIFFERENCE"
        mod.object = cutter
        bpy.context.view_layer.objects.active = wall_obj
        bpy.ops.object.modifier_apply(modifier=mod.name)
    except Exception as e:
        print(f"  Warning: Boolean failed for {opening.get('opening_id')}: {e}")
        # 폴백: 솔리드 벽체 유지
    finally:
        bpy.data.objects.remove(cutter, do_unlink=True)
```

> **변경 v1.1**: (1) `transform_apply` 호출 추가 — Boolean 전 스케일/회전을 메쉬 데이터에 적용하여 좌표계 불일치 방지. (2) 개구부 위치 계산을 벽체 시작점 기반으로 단순화. (3) Boolean 실패 시 `try/except`로 폴백 처리. (4) `create_wall_block` 내부에서 개구부 루프 통합.

**바닥/천장판 생성**:

> **주의**: 인접 방이 공유하는 벽 위치에 이중 면(double face)이 생성되는 것을 방지하기 위해, 개별 방을 먼저 생성한 후 **Boolean Union으로 합집합** 처리한다. 단일 방 건물이거나 방이 겹치지 않으면 개별 생성만으로 충분.

```python
def create_floor_plate(rooms: list, elevation: float,
                        thickness: float = 0.2,
                        label: str = "FloorPlate") -> bpy.types.Object:
    if not rooms:
        return None

    plate_objects = []
    for i, room in enumerate(rooms):
        poly = room["polygon"]
        if len(poly) < 3:
            continue

        bm = bmesh.new()
        bottom_verts = [bm.verts.new((x, y, elevation)) for x, y in poly]
        top_verts = [bm.verts.new((x, y, elevation + thickness)) for x, y in poly]
        bm.verts.ensure_lookup_table()

        n = len(poly)
        bm.faces.new(top_verts)
        bm.faces.new(list(reversed(bottom_verts)))
        for j in range(n):
            k = (j + 1) % n
            bm.faces.new([bottom_verts[j], bottom_verts[k],
                          top_verts[k], top_verts[j]])

        mesh = bpy.data.meshes.new(f"Plate_{i}")
        bm.to_mesh(mesh)
        bm.free()

        obj = bpy.data.objects.new(f"Plate_{i}", mesh)
        bpy.context.collection.objects.link(obj)
        plate_objects.append(obj)

    if len(plate_objects) == 0:
        return None

    # 단일 방이면 Union 불필요
    if len(plate_objects) == 1:
        plate_objects[0].name = label
        return plate_objects[0]

    # 다수 방 → Boolean Union으로 합집합 (이중 면 방지)
    base = plate_objects[0]
    bpy.context.view_layer.objects.active = base
    for other in plate_objects[1:]:
        mod = base.modifiers.new(name="Union", type="BOOLEAN")
        mod.operation = "UNION"
        mod.object = other
        try:
            bpy.ops.object.modifier_apply(modifier=mod.name)
        except Exception:
            pass  # Union 실패 시 개별 유지
        bpy.data.objects.remove(other, do_unlink=True)

    base.name = label
    return base
```

> **변경 v1.1**: 방별 개별 메쉬 생성 후 Boolean Union 합집합. 인접 방 경계의 이중 면 제거.

**계단 블록 생성 규칙**:

| 계단 타입 | 지오메트리 | 치수 산출 |
|----------|----------|---------|
| `straight` | 단일 경사 박스 (ramp) | 폭=size[0], 길이=size[1], 높이=floor_height |
| `180_turn` | 2개 경사 박스 + 참(landing) | 참 폭=size[0], 참 깊이=size[0], 각 경사 길이=size[1]/2 |
| `L_turn` | 2개 경사 박스 + 코너 참 | 코너에서 90° 방향 전환 |
| `spiral` | N개 세그먼트 (N=floor_height/0.2) | 각 세그먼트 360°/N 회전 |

> 블록박스 단계에서 계단은 디딤면 개별 생성이 아닌 **경사면 박스(ramp)** 로 단순화. 플레이어 이동 테스트에 충분한 수준.

### 5.2 교량 블록박스

교량은 `bridges` 배열을 순회하며 모듈 단위로 생성.

**생성 순서**:

```
bridge 데이터 로드
  │
  ├── 1. 데크(상판)
  │    ├── path 경로를 따라 연속 박스 세그먼트 배치
  │    ├── 각 세그먼트: 길이 = path 점 간 거리, 폭 = deck.width_m, 두께 = deck.thickness_m
  │    └── 세그먼트 간 경사 변화 적용 (path Z좌표 기반)
  │
  ├── 2. 인도(sidewalk)
  │    ├── 데크 양쪽 가장자리에 배치
  │    ├── 폭 = sidewalk_width_m, 높이차 = sidewalk_height_offset_m
  │    └── 데크와 동일 경로 추종
  │
  ├── 3. 교각(pier)
  │    ├── station_m 위치에서 데크 중심 아래로 직육면체
  │    └── 상단 = top_z, 하단 = bottom_z
  │
  ├── 4. 교대(abutment)
  │    ├── 양 끝 station_m 위치에 폭이 넓은 직육면체
  │    └── 상단 = top_z, 하단 = bottom_z
  │
  └── 5. 난간(railing)
       ├── 데크 양쪽 가장자리 + sidewalk 바깥쪽
       ├── 높이 = railings.height_m
       └── 포스트: post_spacing_m 간격으로 직육면체
```

**데크 세그먼트 생성**:

```python
def create_bridge_deck(bridge: dict) -> list:
    path = bridge["path"]
    deck = bridge["deck"]
    segments = []

    for i in range(len(path) - 1):
        p0 = np.array(path[i])
        p1 = np.array(path[i + 1])
        mid = (p0 + p1) / 2.0
        seg_length = np.linalg.norm(p1 - p0)
        angle = math.atan2(p1[1] - p0[1], p1[0] - p0[0])
        slope = math.atan2(p1[2] - p0[2], np.linalg.norm(p1[:2] - p0[:2]))

        bpy.ops.mesh.primitive_cube_add(size=1.0, location=tuple(mid))
        obj = bpy.context.active_object
        obj.scale = (seg_length, deck["width_m"], deck["thickness_m"])
        obj.rotation_euler = (slope, 0, angle)
        obj.name = f"Deck_seg_{i}"
        segments.append(obj)

    return segments
```

---

## 7. Blender 스크립트 확장 설계

기존 `blender_extrude.py`와의 관계 및 새 스크립트 구조.

### 파일 구조

```
files/
├── blender_extrude.py             ← 기존: footprint→솔리드 건물 FBX
├── blender_blockbox.py            ← 신규: floorplan.json→블록박스 FBX
├── floorplan_parser.py            ← 신규: DXF/Image→floorplan.json 변환
├── floorplan_postprocess.py       ← 신규: 벽 병합, 방 인식, 개구부 분류
└── config.py                      ← 기존 (건물 타입 맵 등)
```

### blender_blockbox.py 실행 방법

```bash
# 건물 블록박스
blender --background --python blender_blockbox.py -- \
  --input floorplan.json \
  --output output/blockbox/ \
  --mode building

# 교량 블록박스
blender --background --python blender_blockbox.py -- \
  --input floorplan.json \
  --output output/blockbox/ \
  --mode bridge

# 전체 (건물 + 교량)
blender --background --python blender_blockbox.py -- \
  --input floorplan.json \
  --output output/blockbox/ \
  --mode all
```

### floorplan_parser.py 실행 방법

```bash
# DXF → floorplan.json
python floorplan_parser.py \
  --input blueprint.dxf \
  --output floorplan.json \
  --source dxf \
  --floor-height 3.2

# Image → floorplan.json
python floorplan_parser.py \
  --input blueprint.png \
  --output floorplan.json \
  --source image \
  --ppm 50 \
  --floor-height 3.2

# 다층 건물 (층별 이미지)
python floorplan_parser.py \
  --input 1F.png 2F.png 3F.png \
  --output floorplan.json \
  --source image \
  --ppm 50 \
  --floor-height 3.2 3.2 3.0
```

### 주요 함수 시그니처

```python
# ── floorplan_parser.py ──

def parse_dxf_floorplan(dxf_path: str, floor_index: int,
                         floor_height: float,
                         layer_mapping: dict = None) -> dict:
    """DXF 파일 → 단일 층 데이터 dict"""

def detect_walls_from_image(image_path: str, pixels_per_meter: float,
                             wall_thickness_px: int = 3) -> dict:
    """이미지 → 벽체+개구부 raw dict"""


# ── floorplan_postprocess.py ──

def merge_collinear_walls(walls: list, angle_tol: float = 5.0,
                           dist_tol: float = 0.1) -> list:
    """동일선 벽체 세그먼트 병합"""

def detect_rooms(walls: list) -> list:
    """벽 그래프 → 최소순환 → 방 폴리곤 목록"""

def classify_wall_types(walls: list, rooms: list) -> list:
    """인접 방 개수 기반 exterior/interior 재분류"""

def classify_openings(gaps: list, walls: list) -> list:
    """갭 폭/위치 기반 door/window 분류"""

def validate_floorplan(floorplan: dict) -> list:
    """검증: 닫히지 않은 방, 고립 벽, 겹침 등 → 경고 목록"""


# ── blender_blockbox.py ──

def create_wall_block(wall: dict, floor_elevation: float,
                       floor_height: float) -> bpy.types.Object:
    """벽체 → 직육면체 + Boolean 개구부"""

def create_floor_plate(rooms: list, elevation: float,
                        thickness: float = 0.2) -> bpy.types.Object:
    """방 폴리곤 합집합 → 바닥/천장판"""

def create_stair_block(stair: dict, floor_elevation: float,
                        floor_height: float) -> bpy.types.Object:
    """계단 → 경사면 박스"""

def create_column_block(column: dict, floor_elevation: float,
                          floor_height: float) -> bpy.types.Object:
    """기둥 → 직육면체/8각기둥"""

def create_bridge_deck(bridge: dict) -> list:
    """교량 데크 → 세그먼트 목록"""

def create_bridge_piers(bridge: dict) -> list:
    """교각/교대 → 직육면체 목록"""

def create_bridge_railings(bridge: dict) -> list:
    """난간 → 포스트+레일 목록"""

def export_blockbox_fbx(objects: list, filepath: str) -> None:
    """UE5 호환 FBX 내보내기 (blender_extrude.py의 export_fbx_ue5와 동일 설정)"""
```

### 머티리얼 색상 규칙

블록박스는 공간 유형 구분을 위해 색상 코딩 적용.

| 요소 | RGB | 용도 |
|------|-----|------|
| 외벽 (exterior) | `(0.65, 0.65, 0.70)` | 건물 외곽 식별 |
| 내벽 (interior) | `(0.80, 0.75, 0.65)` | 내부 구조벽 |
| 칸막이 (partition) | `(0.85, 0.85, 0.80)` | 경량 벽 |
| 바닥판 | `(0.60, 0.60, 0.55)` | 층 구분 |
| 천장판 | `(0.70, 0.70, 0.68)` | |
| 계단 | `(0.50, 0.70, 0.50)` | 수직 이동 |
| 기둥 | `(0.75, 0.55, 0.55)` | 구조재 |
| 교량 데크 | `(0.55, 0.55, 0.60)` | |
| 교각/교대 | `(0.60, 0.55, 0.50)` | |
| 난간 | `(0.70, 0.70, 0.70)` | |

---

## 8. UE5 네이티브 대안 검토

Blender 외부 파이프라인 대신 UE5 내부에서 직접 블록박스를 생성하는 옵션.

| 방식 | 장점 | 단점 | 판정 |
|------|------|------|------|
| **ProceduralMeshComponent** | 즉시 반영, 에디터 내 실시간 미리보기 | 콜리전 수동 설정, HISM 불가, 저장 시 직렬화 필요 | Phase 2 고려 |
| **RuntimeMeshComponent** (플러그인) | ProceduralMesh 상위호환, LOD 지원 | 외부 플러그인 의존 | 선택 사항 |
| **BSP Brush** | UE5 내장, 콜리전 자동 | 대량 생성 시 성능 저하, Geometry Script와 충돌 가능 | 소규모만 |
| **Geometry Script** | UE5.1+ 내장, 노드 기반 프로시저럴 | 학습 곡선, JSON 입력 파싱 복잡 | 장기 목표 |
| **Blender → FBX (본 문서)** | 검증된 파이프라인, 기존 코드 재활용 | 왕복 시간 (생성→임포트), 이터레이션 느림 | **Phase 1 채택** |

**권장 전략**:

- **Phase 1**: Blender 스크립트로 FBX 생성 → UE5 Import (검증된 경로)
- **Phase 2**: UE5 ProceduralMesh로 에디터 내 실시간 프리뷰 추가
- **Phase 3**: Geometry Script로 완전 네이티브화 (선택)

---

## 9. 기존 시스템 통합

### blender_extrude.py와의 관계

| 항목 | blender_extrude.py | blender_blockbox.py (신규) |
|------|-------------------|--------------------------|
| 입력 | buildings.json (footprint + height) | floorplan.json (벽·방·개구부·계단) |
| 출력 | 솔리드 건물 FBX (외벽만) | 블록박스 FBX (내외부 + 개구부) |
| 메쉬 유형 | footprint 압출 단일 메쉬 | 벽·바닥·천장·계단 개별 메쉬 |
| 용도 | 맵 전체 건물 스카이라인 | 대형 구조물 내부 테스트 |
| 공유 코드 | — | `export_fbx_ue5()`, 머티리얼 유틸, 씬 설정 |

### StructureGenerator와의 연동

`floorplan.json` → `structure_layout.json` 변환 매핑:

| floorplan.json | structure_layout.json | 변환 규칙 |
|---------------|----------------------|----------|
| `floors[].rooms[]` | `floors[].zones[].rooms[]` | room.usage → zone_type 매핑 |
| `walls[].openings[]` | `rooms[].doors[]` | opening.type=door → door 항목 |
| `stairs[]` | `stairs[]` | 직접 매핑 (필드명 호환) |
| `columns[]` | `cover_hints[]` | 기둥 위치 → 엄폐 후보 자동 등록 |
| `rooms[].polygon` | `rooms[].aabb_ue5` | 폴리곤 → AABB 변환 (meter → cm) |

### UE5 FBX Import 규칙

| 설정 | 값 | 이유 |
|------|-----|------|
| Import Scale | 1.0 | Blender에서 cm 스케일로 출력 |
| Combine Meshes | No | 벽·바닥·계단 개별 Actor 유지 (편집 가능) |
| Auto Generate Collision | Yes | 블록박스에 자동 콜리전 적용 |
| Import as Skeletal | No | 스태틱 메쉬만 |
| Material Import | Do Not Create | UE5 측 블록박스 머티리얼 수동 할당 |

---

## 10. 에러 핸들링 및 검증

### floorplan.json 검증 규칙

유효한 `floorplan.json` 파일의 조건. `validate_floorplan()` 함수가 아래 규칙을 검사.

| # | 규칙 | 심각도 | 자동 보정 |
|---|------|--------|----------|
| V-1 | `version` 필드 존재 + 지원 버전 | Error | 불가 |
| V-2 | 모든 `wall_id`가 파일 내 고유 | Error | 접미사 자동 부여 (`_dup1`) |
| V-3 | 벽체 `start` ≠ `end` (길이 0 벽 금지) | Error | 삭제 + Warning |
| V-4 | `thickness_m` > 0 | Error | 기본값 0.15 적용 |
| V-5 | 개구부 `offset_m + width_m` ≤ 벽 길이 | Warning | offset 클램핑 |
| V-6 | 개구부 `height_m` ≤ `floor.height_m` | Warning | height 클램핑 |
| V-7 | `room.polygon`이 닫힌 폴리곤 (첫점≈끝점 or 3점 이상) | Warning | 강제 닫기 |
| V-8 | `room.boundary_wall_ids` 내 벽 ID가 모두 존재 | Warning | 미존재 ID 제거 |
| V-9 | `stairs.connects_floors` 내 floor_index가 모두 존재 | Warning | 미존재 층 제거 |
| V-10 | 벽체 교차점에서 끝점 오차 < 0.1m | Info | 자동 스냅 |
| V-11 | 방 면적 > 0 (시계/반시계 방향 검사) | Warning | 폴리곤 방향 반전 |
| V-12 | 교량 `path` 최소 2점 | Error | 불가 |
| V-13 | 교량 `pier.station_m`이 path 전체 길이 이내 | Warning | 클램핑 |

### 파싱 에러 처리

| 상황 | 처리 | 폴백 |
|------|------|------|
| DXF 파일 읽기 실패 | `ezdxf.DXFError` catch → 에러 메시지 + 종료 | — |
| DXF에 WALL 레이어 없음 | 모든 LINE/LWPOLYLINE을 벽 후보로 처리 | Warning: "WALL 레이어 미발견. 전체 LINE을 벽으로 해석" |
| 이미지 로드 실패 | `cv2.imread` None 체크 → 에러 + 종료 | — |
| 벽체 0개 검출 | 에러 + "이미지 해상도/대비 확인 필요" 메시지 | — |
| HoughLines 과다 검출 (>1000) | threshold 자동 상향 (50→100→150) 재시도 | 최대 3회 재시도 |
| 방 인식 0개 | Warning: "닫힌 방을 찾지 못함. 벽 연결 확인 필요" | 벽만 블록박스화 (방 없이) |
| Boolean 연산 실패 (Blender) | 개구부 스킵 + Warning | 솔리드 벽체 유지 |
| FBX 내보내기 실패 | Blender 에러 로그 출력 + 종료 | — |

### 검증 리포트 출력

```
=== Floorplan Validation Report ===
  File: floorplan_factory_sample.json
  Version: 1.0

  Floors: 2
  Total walls: 18  (exterior: 8, interior: 5, partition: 5)
  Total rooms: 8
  Total openings: 13  (door: 8, window: 3, shutter: 2)
  Total stairs: 2
  Total columns: 6

  Errors:   0
  Warnings: 1
    [V-5] Wall w_003: opening op_006 offset+width (29.5) > wall length (60.0) — OK
  Info:     2
    [V-10] Snapped 3 wall endpoints (max delta: 0.05m)
    [V-11] Room r_001 polygon reoriented to CCW

  Result: PASS (0 errors)
===
```

---

## 11. 성능 예산

| 항목 | 목표 | 근거 |
|------|------|------|
| **DXF 파싱** | < 2초 / 파일 | ezdxf는 100K entity DXF를 ~1초에 파싱 (벤치마크) |
| **이미지 벽체 검출** | < 5초 / 이미지 | 4000×3000px 기준. HoughLinesP가 지배적 |
| **후처리 (방 인식 포함)** | < 3초 / 층 | 최소순환 탐색이 지배적. 벽 500개 이하 가정 |
| **Blender 벽체 생성** | ~50ms / 벽 (Boolean 포함) | `transform_apply` + Boolean DIFFERENCE |
| **Blender 바닥판 생성** | ~200ms / 층 | Boolean Union (방 10개 기준) |
| **Blender 교량 데크** | ~100ms / 세그먼트 | 단순 cube 배치 |
| **FBX 내보내기** | < 3초 / 건물 | 3층 건물 (벽 50개 + 바닥 6개 + 계단 4개) |
| **전체 파이프라인** (DXF→FBX) | < 30초 / 건물 | 5층 이하, 벽 200개 이하 |
| **FBX 파일 크기** | < 5MB / 건물 | 블록박스는 저폴리곤 (건물당 ~2K tri) |
| **UE5 Import** | < 10초 / FBX | Auto collision 포함 |

**스케일 제한**:

| 제한 | 값 | 초과 시 |
|------|-----|--------|
| 단일 건물 최대 벽 수 | 500 | Warning: "대규모 건물. 생성 시간이 길어질 수 있음" |
| 단일 건물 최대 층수 | 10 | Warning + Boolean 최적화 모드 (개구부 생략 옵션) |
| 단일 건물 최대 개구부 | 300 | Boolean 배치 모드: 동일 벽의 개구부를 하나의 커터로 합침 |
| 교량 최대 path 길이 | 1km | 세그먼트 수 제한: 100개까지 |

---

## 12. 다층 건물 정합 규칙

다층 건물에서 층간 데이터 일관성을 보장하는 규칙.

### 벽체 정렬

| 규칙 | 설명 | 처리 |
|------|------|------|
| **외벽 정렬** | 모든 층의 `exterior` 벽은 동일한 footprint 내에 있어야 함 | 1층 외벽을 기준으로 상층 외벽 검증. 오차 0.3m 초과 시 Warning |
| **구조벽 관통** | `interior` 벽이 다층에 걸쳐 동일 위치에 있으면 구조벽 | 자동 태깅: 2개 이상 층에서 위치 일치하는 내벽 → `structural: true` |
| **칸막이 독립** | `partition` 벽은 층별 독립 (정렬 불요) | 검증 대상에서 제외 |

### 계단 관통

```
계단이 connects_floors: [0, 1, 2]인 경우:

  ┌─ 3F (floor_index=2) ─────────┐
  │  계단 개구부 (floor plate에 구멍)│
  │  + 경사면 상단                  │
  ├─ 2F (floor_index=1) ─────────┤
  │  계단 개구부 (바닥+천장 관통)    │
  │  + 경사면 중간                  │
  ├─ 1F (floor_index=0) ─────────┤
  │  경사면 하단                    │
  │  바닥판은 유지 (1층 바닥)        │
  └───────────────────────────────┘
```

**처리 규칙**:
- 계단이 관통하는 중간 층의 바닥판/천장판에 계단 크기만큼 **구멍(hole)** 생성
- 구멍 크기 = `stair.size_m` + 여유 0.1m (각 방향)
- 최하층 바닥판은 구멍 없음 (지면)
- 최상층 천장판(옥상)은 구멍 없음

### 엘리베이터 관통

계단과 동일 규칙. `elevator.size_m`만큼 바닥/천장 관통. 엘리베이터 샤프트 벽은 자동 생성 (4면 얇은 벽).

### 층간 높이 누적

```
floor[0].elevation_m = 0.0
floor[1].elevation_m = floor[0].elevation_m + floor[0].height_m
floor[2].elevation_m = floor[1].elevation_m + floor[1].height_m
...
```

검증: `floor[n].elevation_m`이 이전 층의 `elevation_m + height_m`과 일치하는지 확인. 불일치 시 Warning + 자동 보정 옵션.

---

## 13. 구현 순서 및 일정

| Phase | 내용 | 산출물 | 예상 기간 |
|-------|------|--------|----------|
| **F-0** | `floorplan.json` 스키마 확정 + 샘플 2종 작성 | 스키마 문서, factory_sample.json, school_sample.json | 1일 | ✅ 완료 |
| **F-1a** | DXF 파서 구현 | `floorplan_parser.py` (DXF 모드) | 3~4일 | ✅ 완료 |
| **F-1b** | 이미지 벽체 검출 구현 | `floorplan_parser.py` (Image 모드) | 4~5일 | ✅ 완료 |
| **F-1c** | 후처리 파이프라인 구현 | `floorplan_postprocess.py` | 3~4일 | ✅ 완료 |
| **F-1d** | 파라미터 기반 생성기 구현 (Track C) | `floorplan_generator.py` | 2~3일 | ✅ 완료 |
| **F-2** | 건물 블록박스 Blender 스크립트 | `blender_blockbox.py` (building 모드) | 3~4일 | ✅ 완료 |
| **F-3** | 교량 블록박스 Blender 스크립트 | `blender_blockbox.py` (bridge 모드) | 2~3일 | ✅ 완료 |
| **F-4** | UE5 Import 가이드 + FBX 설정 | Import 프리셋, 테스트 가이드 | 2일 | ✅ 완료 |
| **F-5** | structure_layout.json 변환기 | `floorplan_to_structure.py` | 2일 | ✅ 완료 |

**의존 관계**:

```
F-0 ──→ F-1a ──┐
         F-1b ──├→ F-1c ──→ F-2 ──→ F-4 ──→ F-5
         F-1c ──┘           F-3 ──→ F-4
         F-1d ──────────────↗ (독립. F-0 이후 바로 착수 가능)
```

**테스트 기준**:

| Phase | Pass 기준 |
|-------|----------|
| F-0 | JSON Schema 검증 통과. 두 샘플 모두 유효 |
| F-1a | 테스트 DXF에서 벽체 90%+ 추출, 문/창 80%+ 추출 |
| F-1b | 테스트 이미지에서 벽체 85%+ 검출, 위치 오차 ≤ 0.3m |
| F-1c | 방 인식 정확도 80%+, 닫히지 않은 방 0개 (후보정 후) |
| F-1d | 5가지 프리셋(병원/학교/공장/오피스/창고)의 JSON 생성 성공 + 시각화 정상 |
| F-2 | 3층 건물 블록박스 FBX 생성 성공. UE5 Import 후 걸어다닐 수 있음 |
| F-3 | 100m 빔 교량 블록박스 FBX 생성 성공. UE5에서 차량 통행 가능 스케일 |
| F-4 | UE5에서 건물 내부 진입 + 층간 이동 + 교량 통행 확인 |
| F-5 | 변환된 structure_layout.json이 StructureGenerator 에디터에서 로드 성공 |

---

## 14. 기획 확정 사항

| # | 항목 | 확정 방향 | 버전 |
|---|------|-----------|------|
| 1 | 표준 교환 포맷 | `floorplan.json` v1.0 스키마 | v1.0 |
| 2 | 입력 소스 | DXF/SVG (벡터), PNG/JPG (이미지), 수동 편집, **파라미터 생성** | v1.1 |
| 3 | DXF 파싱 | `ezdxf` — LINE/LWPOLYLINE/ARC/CIRCLE/INSERT 지원 | v1.0 |
| 4 | 이미지 검출 | OpenCV — Adaptive Threshold + Morphology + HoughLinesP | v1.0 |
| 5 | 벽체 후처리 | T-junction 분할, 동일선 병합, 직교 스냅, 끝점 스냅 | v1.0 |
| 6 | 방 인식 | 벽 그래프 최소순환 탐색. 면적 1~500m² 필터 | v1.0 |
| 7 | 벽 유형 분류 | 인접 방 개수 기반: 1면=exterior, 2면=interior | v1.0 |
| 8 | 개구부 분류 | Gap 폭 기반: 0.7~1.5m=door, 0.5~3.0m+중간=window | v1.0 |
| 9 | 블록박스 생성 | Blender 스크립트 (`blender_blockbox.py`) | v1.0 |
| 10 | 벽체 메쉬 | 벽 중심선 → 직육면체 + Boolean 개구부 | v1.0 |
| 11 | 바닥/천장판 | 방 polygon 기반 평면 메쉬 (두께 0.2m) | v1.0 |
| 12 | 계단 블록 | 경사면 박스(ramp) 단순화. 4종 타입 | v1.0 |
| 13 | 기둥 블록 | 직육면체 또는 8각기둥 | v1.0 |
| 14 | 교량 블록박스 | 데크+교각+교대+인도+난간 모듈 조립 | v1.0 |
| 15 | FBX 출력 | UE5 호환 (Forward -Z, Up Y, Scale 1.0) | v1.0 |
| 16 | 색상 코딩 | 요소 유형별 10색 머티리얼 | v1.0 |
| 17 | 기존 코드 공유 | `export_fbx_ue5()` 함수 재사용 | v1.0 |
| 18 | StructureGenerator 연동 | floorplan.json ↔ structure_layout.json 양방향 변환 | v1.0 |
| 19 | UE5 네이티브 | Phase 2 ProceduralMesh, Phase 3 Geometry Script | v1.0 |
| 20 | 구현 Phase | F-0→F-1a/b/c/d→F-2→F-3→F-4→F-5 | v1.1 |
| 21 | 이미지 벽체 검출 정확도 목표 | ≥ 85% | v1.0 |
| 22 | DXF 레이어 매핑 | 커스터마이즈 가능한 `layer_mapping.json` | v1.0 |
| 23 | 좌표계 | 2D [x,y] 미터, Z-up. FBX 내보내기에서 UE5 변환 | v1.0 |
| 24 | 검증 규칙 | 13개 규칙 (V-1~V-13). Error/Warning/Info 3단계 | v1.0 |
| 25 | 에러 핸들링 | 8가지 파싱 에러 케이스 + 폴백 전략 정의 | v1.0 |
| 26 | 다층 정합 | 외벽 정렬 검증, 계단/EV 관통 구멍, 층간 높이 누적 검증 | v1.0 |
| 27 | 샘플 데이터 | 공장 2층 + 학교 3층 + 교량 2종 + L자 병원 (4파일) | v1.1 |
| 28 | 파라미터 생성 (Track C) | 도면 없이 설정값으로 floorplan.json 프로시저럴 생성 | v1.1 |
| 29 | Track C 필수 파라미터 | width, depth, floors, rooms, windows, door_placement | v1.1 |
| 30 | Track C 건물 형태 | rectangle / L / T / U 지원 | v1.1 |
| 31 | Track C 복도 유형 | none / single / double / L / loop | v1.1 |

---

## 변경 내역

| 버전 | 날짜 | 내용 |
|------|------|------|
| v1.0 | 2026-03-20 | 초안 작성. 12개 섹션 + 기획 확정 사항 27항. 샘플 JSON 3종 포함 |
| v1.1 | 2026-03-20 | P0~P1 보완 + Track C(파라미터 생성) 추가. 14개 섹션 + 확정 31항. 샘플 5종 + layer_mapping |

---

— 기획문서 끝 (v1.1) —
