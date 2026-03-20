# LevelTool — Reference Map Generation 기획문서 v1.0

> **이전 문서**: `LevelTool_DesignerIntent_보완문서.md` v4.4 (2~3단계 슬라이더/진단 기반 → **폐기**)  
> **변경 사유**: 슬라이더/프리셋/진단 시스템을 레퍼런스 게임 맵 이미지 기반 직접 생성으로 전면 교체  
> **핵심 변경**: "추상적 수치 조작" → "레퍼런스 맵 복제 수준 생성"

---

## 목차

1. [기획의도 — 새 구조](#1-기획의도--새-구조)
2. [전체 아키텍처](#2-전체-아키텍처)
3. [레퍼런스 데이터 구조](#3-레퍼런스-데이터-구조)
4. [BR 생성 파이프라인](#4-br-생성-파이프라인)
5. [EX 생성 파이프라인](#5-ex-생성-파이프라인)
6. [내장 레퍼런스 — Battle Royale](#6-내장-레퍼런스--battle-royale)
7. [내장 레퍼런스 — Extraction](#7-내장-레퍼런스--extraction)
8. [건물 스타일 프로파일](#8-건물-스타일-프로파일)
9. [이미지 분석 엔진 (UE5 C++)](#9-이미지-분석-엔진-ue5-c)
10. [에디터 UI](#10-에디터-ui)
11. [기존 시스템과의 관계](#11-기존-시스템과의-관계)
12. [구현 순서](#12-구현-순서)
13. [레퍼런스 맵 상세 데이터](#13-레퍼런스-맵-상세-데이터)

---

## 1. 기획의도 — 새 구조

### 이전 구조 (폐기)

```
[1단계: OSM 좌표 기반 생성] → [2단계: 슬라이더/프리셋 수정] ⇄ [3단계: 룰셋 진단/제안]
```

**폐기 사유**: 슬라이더 수치("도심 밀도 65")가 추상적이고, 결과를 예측하기 어려우며, "뭐가 바뀐 건지 모르겠다"는 피드백 발생

### 새 구조

```
[1단계: OSM 좌표 기반 생성 (유지)]
[신규: 레퍼런스 맵 생성]
  ├── Battle Royale → 내장 이미지 + POI 메타 → 맵 생성
  └── Extraction   → 내장 이미지 + POI 메타 → 맵 생성 (별도 규칙)
```

### 핵심 목표

| 항목 | 내용 |
|------|------|
| **입력** | 레퍼런스 게임의 탑뷰 맵 이미지 (클린, 텍스트 없는 버전) + POI 메타데이터 JSON |
| **출력** | UE5 월드에 "알아볼 수 있는 수준"으로 복제된 맵 (해안선, 도시 배치, 도로, 지형) |
| **목표 유사도** | 70~80% — 형태를 알아볼 수 있고 건물 위치가 대략 유사 |
| **확장성** | 기획자가 pois.json / styles.json을 편집해서 품질 향상 가능 |

---

## 2. 전체 아키텍처

```
레퍼런스 데이터 (플러그인 Content 폴더)
├── BR/
│   ├── Erangel/
│   │   ├── map_clean.png      ← 텍스트 없는 탑뷰 이미지
│   │   ├── map_meta.json      ← 맵 크기, 육지 비율, 기본 설정
│   │   ├── pois.json          ← POI 목록 (위치, 유형, 등급, 건물 수, 고도 힌트)
│   │   └── roads_hint.json    ← [선택] 도로 보정 데이터
│   ├── Miramar/ ...
│   └── Verdansk/ ...
├── EX/
│   ├── Customs/
│   │   ├── map_clean.png
│   │   ├── map_meta.json
│   │   ├── pois.json
│   │   └── routes.json        ← 추출 포인트, 핵심 동선
│   └── Reserve/ ...
└── Styles/
    └── building_styles.json   ← 건물 스타일 프로파일 (공용)
```

### 생성 플로우

```
[사용자: 맵 선택 (예: Erangel)] → [버튼: 생성]
    │
    ├── 1. 이미지 분석 (UE5 C++)
    │   ├── 육지/수역 마스크 추출
    │   ├── Heightmap 추출 (밝기 → 고도)
    │   └── 도로 라인 추출 (밝은 선 감지)
    │
    ├── 2. 메타데이터 병합
    │   ├── POI 위치/크기/유형 → 건물 배치 계획
    │   ├── 고도 힌트 → heightmap 보정
    │   └── 스타일 프로파일 → 건물 높이/유형 결정
    │
    └── 3. UE5 월드 생성
        ├── Landscape 생성 (heightmap 적용)
        ├── 수역 생성 (마스크 기반)
        ├── 도로 생성 (추출 경로 + 메타 보정)
        └── 건물 배치 (POI별 스타일 기반 절차적 배치)
```

---

## 3. 레퍼런스 데이터 구조

### 3.1 map_meta.json

```json
{
  "version": "1.0",
  "name": "Erangel",
  "game": "PUBG",
  "ruleset": "BR",
  "size_km": [8, 8],
  "land_ratio": 0.5147,
  "water_ratio": 0.4853,
  "player_count": 100,
  "elevation_range_m": [0, 200],
  "theme": "temperate_island",
  "image_file": "map_clean.png",
  "notes": "동유럽풍 섬. 남쪽 Military Island 다리 2개로 연결"
}
```

### 3.2 pois.json

```json
{
  "version": "1.0",
  "pois": [
    {
      "name": "Pochinki",
      "name_kr": "포친키",
      "center_pct": [0.42, 0.52],
      "radius_m": 300,
      "type": "town",
      "style": "eastern_rural",
      "loot_tier": "S",
      "building_count": 42,
      "avg_floors": 2,
      "elevation_hint": "flat",
      "elevation_m": 30,
      "description": "맵 정중앙 대도시. 밀집 시가전",
      "landmarks": ["church", "3story_row"],
      "connections": ["Rozhok", "School", "Gatka"]
    },
    {
      "name": "Sosnovka Military Base",
      "name_kr": "밀리터리 베이스",
      "center_pct": [0.52, 0.85],
      "radius_m": 500,
      "type": "military_base",
      "style": "military",
      "loot_tier": "S",
      "building_count": 30,
      "avg_floors": 1,
      "elevation_hint": "flat",
      "elevation_m": 5,
      "description": "분리된 섬. 다리 2개로만 진입. 최대 병목",
      "landmarks": ["radar_tower", "runway", "barracks_u"],
      "connections": [],
      "island": true,
      "bridge_connections": ["main_island_south"]
    },
    {
      "name": "Georgopol",
      "name_kr": "게오르고폴",
      "center_pct": [0.18, 0.35],
      "radius_m": 400,
      "type": "port_city",
      "style": "port_industrial",
      "loot_tier": "A",
      "building_count": 80,
      "avg_floors": 3,
      "elevation_hint": "coastal",
      "elevation_m": 10,
      "description": "만 기준 남북 분리 대도시. 컨테이너 항구",
      "landmarks": ["container_yard", "apartment_9floor"],
      "connections": ["Zharki", "Rozhok"]
    },
    {
      "name": "Yasnaya Polyana",
      "name_kr": "야스나야 폴리아나",
      "center_pct": [0.72, 0.28],
      "radius_m": 350,
      "type": "town",
      "style": "dense_urban",
      "loot_tier": "A",
      "building_count": 60,
      "avg_floors": 3,
      "elevation_hint": "gentle_hill",
      "elevation_m": 50,
      "description": "북동쪽 대도시. 고층 감옥 건물 요충지",
      "landmarks": ["prison_tower", "church"],
      "connections": ["Polyana", "Stalber"]
    },
    {
      "name": "Rozhok",
      "name_kr": "로죽",
      "center_pct": [0.45, 0.35],
      "radius_m": 200,
      "type": "town",
      "style": "eastern_rural",
      "loot_tier": "B",
      "building_count": 25,
      "avg_floors": 2,
      "elevation_hint": "gentle_hill",
      "elevation_m": 40,
      "description": "중앙 소도시. School과 인접",
      "landmarks": ["church"],
      "connections": ["School", "Pochinki", "Georgopol"]
    },
    {
      "name": "School",
      "name_kr": "학교",
      "center_pct": [0.47, 0.38],
      "radius_m": 100,
      "type": "landmark",
      "style": "institutional",
      "loot_tier": "S",
      "building_count": 5,
      "avg_floors": 3,
      "elevation_hint": "flat",
      "elevation_m": 35,
      "description": "대형 단일 건물. 다층 CQB",
      "landmarks": ["school_main", "apartments_adjacent"],
      "connections": ["Rozhok", "Pochinki"]
    },
    {
      "name": "Mylta Power",
      "name_kr": "밀타 발전소",
      "center_pct": [0.85, 0.65],
      "radius_m": 150,
      "type": "industrial",
      "style": "power_plant",
      "loot_tier": "A",
      "building_count": 15,
      "avg_floors": 2,
      "elevation_hint": "coastal",
      "elevation_m": 5,
      "description": "남동쪽 발전소. 좁은 면적 고밀도 루트",
      "landmarks": ["chimney_stack", "factory_hall"],
      "connections": ["Mylta"]
    },
    {
      "name": "Stalber",
      "name_kr": "스탈버",
      "center_pct": [0.72, 0.12],
      "radius_m": 80,
      "type": "watchtower",
      "style": "military_outpost",
      "loot_tier": "C",
      "building_count": 5,
      "avg_floors": 1,
      "elevation_hint": "hilltop",
      "elevation_m": 180,
      "description": "북동쪽 고지대 감시탑",
      "landmarks": ["watchtower"],
      "connections": ["Kameshki"]
    },
    {
      "name": "Zharki",
      "name_kr": "자르키",
      "center_pct": [0.08, 0.08],
      "radius_m": 150,
      "type": "village",
      "style": "eastern_rural",
      "loot_tier": "C",
      "building_count": 12,
      "avg_floors": 1,
      "elevation_hint": "coastal",
      "elevation_m": 5,
      "description": "북서쪽 끝 마을. 안전하지만 자기장 취약",
      "landmarks": [],
      "connections": ["Severny", "Georgopol"]
    },
    {
      "name": "Severny",
      "name_kr": "세버니",
      "center_pct": [0.30, 0.08],
      "radius_m": 150,
      "type": "village",
      "style": "eastern_rural",
      "loot_tier": "C",
      "building_count": 15,
      "avg_floors": 1,
      "elevation_hint": "coastal",
      "elevation_m": 5,
      "description": "북쪽 해안 마을",
      "landmarks": [],
      "connections": ["Zharki", "Shooting Range"]
    },
    {
      "name": "Shooting Range",
      "name_kr": "사격장",
      "center_pct": [0.35, 0.15],
      "radius_m": 100,
      "type": "military_outpost",
      "style": "military",
      "loot_tier": "B",
      "building_count": 8,
      "avg_floors": 1,
      "elevation_hint": "gentle_hill",
      "elevation_m": 60,
      "description": "북쪽 사격 훈련장",
      "landmarks": ["firing_range"],
      "connections": ["Severny", "Stalber"]
    },
    {
      "name": "Hospital",
      "name_kr": "병원",
      "center_pct": [0.13, 0.48],
      "radius_m": 100,
      "type": "landmark",
      "style": "institutional",
      "loot_tier": "B",
      "building_count": 4,
      "avg_floors": 3,
      "elevation_hint": "flat",
      "elevation_m": 20,
      "description": "서쪽 대형 병원 건물",
      "landmarks": ["hospital_main"],
      "connections": ["Georgopol", "Gatka"]
    },
    {
      "name": "Gatka",
      "name_kr": "가트카",
      "center_pct": [0.25, 0.55],
      "radius_m": 150,
      "type": "village",
      "style": "eastern_rural",
      "loot_tier": "C",
      "building_count": 12,
      "avg_floors": 1,
      "elevation_hint": "flat",
      "elevation_m": 20,
      "description": "서쪽 소규모 마을",
      "landmarks": [],
      "connections": ["Hospital", "Pochinki"]
    },
    {
      "name": "Prison",
      "name_kr": "교도소",
      "center_pct": [0.65, 0.60],
      "radius_m": 120,
      "type": "institutional",
      "style": "institutional",
      "loot_tier": "B",
      "building_count": 8,
      "avg_floors": 2,
      "elevation_hint": "hilltop",
      "elevation_m": 80,
      "description": "동쪽 고지대 교도소",
      "landmarks": ["prison_main"],
      "connections": ["Mansion"]
    },
    {
      "name": "Mansion",
      "name_kr": "맨션",
      "center_pct": [0.72, 0.52],
      "radius_m": 80,
      "type": "landmark",
      "style": "eastern_rural",
      "loot_tier": "B",
      "building_count": 3,
      "avg_floors": 2,
      "elevation_hint": "gentle_hill",
      "elevation_m": 50,
      "description": "동쪽 대저택 + Shelter 인접",
      "landmarks": ["mansion_main"],
      "connections": ["Prison", "Shelter"]
    },
    {
      "name": "Lipovka",
      "name_kr": "리포프카",
      "center_pct": [0.80, 0.42],
      "radius_m": 150,
      "type": "village",
      "style": "eastern_rural",
      "loot_tier": "C",
      "building_count": 15,
      "avg_floors": 1,
      "elevation_hint": "coastal",
      "elevation_m": 10,
      "description": "동쪽 해안 마을",
      "landmarks": [],
      "connections": ["Yasnaya Polyana", "Prison"]
    },
    {
      "name": "Quarry",
      "name_kr": "채석장",
      "center_pct": [0.18, 0.68],
      "radius_m": 120,
      "type": "industrial",
      "style": "quarry",
      "loot_tier": "C",
      "building_count": 5,
      "avg_floors": 1,
      "elevation_hint": "valley",
      "elevation_m": -10,
      "description": "남서쪽 분지 채석장. 고지대 선점 필수",
      "landmarks": ["quarry_pit"],
      "connections": ["Primorsk"]
    },
    {
      "name": "Primorsk",
      "name_kr": "프리모르스크",
      "center_pct": [0.18, 0.78],
      "radius_m": 200,
      "type": "town",
      "style": "eastern_rural",
      "loot_tier": "B",
      "building_count": 20,
      "avg_floors": 2,
      "elevation_hint": "coastal",
      "elevation_m": 5,
      "description": "남서쪽 해안 도시",
      "landmarks": [],
      "connections": ["Quarry", "Ferry Pier"]
    },
    {
      "name": "Ferry Pier",
      "name_kr": "페리 항구",
      "center_pct": [0.35, 0.82],
      "radius_m": 100,
      "type": "port",
      "style": "port_industrial",
      "loot_tier": "B",
      "building_count": 8,
      "avg_floors": 1,
      "elevation_hint": "coastal",
      "elevation_m": 2,
      "description": "남쪽 항구. Military Island 다리 연결",
      "landmarks": ["ferry_dock"],
      "connections": ["Primorsk", "Sosnovka Military Base"]
    },
    {
      "name": "Novorepnoye",
      "name_kr": "노보레프노예",
      "center_pct": [0.88, 0.82],
      "radius_m": 200,
      "type": "port_city",
      "style": "port_industrial",
      "loot_tier": "A",
      "building_count": 35,
      "avg_floors": 2,
      "elevation_hint": "coastal",
      "elevation_m": 3,
      "description": "남동쪽 항구 도시. 컨테이너 밀집",
      "landmarks": ["container_yard", "crane"],
      "connections": ["Mylta Power"]
    },
    {
      "name": "Kameshki",
      "name_kr": "카메쉬키",
      "center_pct": [0.92, 0.10],
      "radius_m": 120,
      "type": "village",
      "style": "eastern_rural",
      "loot_tier": "C",
      "building_count": 10,
      "avg_floors": 1,
      "elevation_hint": "coastal",
      "elevation_m": 5,
      "description": "북동쪽 끝 마을. 극동 위치로 자기장 취약",
      "landmarks": [],
      "connections": ["Stalber"]
    }
  ]
}
```

### 3.3 building_styles.json

```json
{
  "version": "1.0",
  "styles": {
    "eastern_rural": {
      "label": "동유럽 농촌",
      "floor_distribution": { "1": 0.50, "2": 0.30, "3": 0.15, "4": 0.05 },
      "type_distribution": {
        "house": 0.45,
        "barn": 0.15,
        "shop": 0.20,
        "church": 0.05,
        "garage": 0.15
      },
      "footprint_m2_range": [40, 150],
      "spacing_m": 15,
      "color_palette": "grey_plaster_red_roof"
    },
    "port_industrial": {
      "label": "항구/산업",
      "floor_distribution": { "1": 0.40, "2": 0.25, "3": 0.20, "5": 0.10, "9": 0.05 },
      "type_distribution": {
        "warehouse": 0.30,
        "container": 0.20,
        "crane": 0.05,
        "apartment": 0.25,
        "office": 0.15,
        "dock": 0.05
      },
      "footprint_m2_range": [60, 400],
      "spacing_m": 10,
      "color_palette": "concrete_rust"
    },
    "military": {
      "label": "군사 시설",
      "floor_distribution": { "1": 0.60, "2": 0.30, "3": 0.10 },
      "type_distribution": {
        "barracks": 0.35,
        "bunker": 0.20,
        "hangar": 0.15,
        "tower": 0.10,
        "fence": 0.10,
        "admin": 0.10
      },
      "footprint_m2_range": [50, 500],
      "spacing_m": 20,
      "color_palette": "military_green_concrete"
    },
    "dense_urban": {
      "label": "도심 밀집",
      "floor_distribution": { "2": 0.15, "3": 0.30, "4": 0.25, "5": 0.20, "8": 0.10 },
      "type_distribution": {
        "apartment": 0.40,
        "shop": 0.25,
        "office": 0.20,
        "church": 0.05,
        "parking": 0.10
      },
      "footprint_m2_range": [80, 300],
      "spacing_m": 8,
      "color_palette": "mixed_urban"
    },
    "institutional": {
      "label": "공공 시설",
      "floor_distribution": { "2": 0.30, "3": 0.40, "4": 0.30 },
      "type_distribution": {
        "main_building": 0.50,
        "annex": 0.30,
        "yard_wall": 0.20
      },
      "footprint_m2_range": [200, 800],
      "spacing_m": 25,
      "color_palette": "concrete_beige"
    },
    "military_outpost": {
      "label": "군사 전초기지",
      "floor_distribution": { "1": 0.70, "2": 0.20, "3": 0.10 },
      "type_distribution": {
        "watchtower": 0.20,
        "small_bunker": 0.30,
        "tent": 0.30,
        "comm_tower": 0.20
      },
      "footprint_m2_range": [20, 100],
      "spacing_m": 15,
      "color_palette": "camo_green"
    },
    "power_plant": {
      "label": "발전소/공장",
      "floor_distribution": { "1": 0.30, "2": 0.30, "3": 0.20, "5": 0.20 },
      "type_distribution": {
        "factory_hall": 0.30,
        "chimney": 0.10,
        "control_room": 0.20,
        "storage_tank": 0.20,
        "pipe_rack": 0.20
      },
      "footprint_m2_range": [100, 600],
      "spacing_m": 20,
      "color_palette": "industrial_grey"
    },
    "quarry": {
      "label": "채석장/광산",
      "floor_distribution": { "1": 0.80, "2": 0.20 },
      "type_distribution": {
        "shed": 0.40,
        "conveyor": 0.20,
        "office_small": 0.20,
        "storage": 0.20
      },
      "footprint_m2_range": [30, 150],
      "spacing_m": 25,
      "color_palette": "dust_brown"
    },
    "extraction_industrial": {
      "label": "익스트랙션 산업 시설",
      "floor_distribution": { "1": 0.30, "2": 0.35, "3": 0.25, "4": 0.10 },
      "type_distribution": {
        "factory": 0.30,
        "office": 0.25,
        "warehouse": 0.25,
        "guard_post": 0.10,
        "container": 0.10
      },
      "footprint_m2_range": [60, 400],
      "spacing_m": 12,
      "color_palette": "industrial_grey"
    },
    "extraction_residential": {
      "label": "익스트랙션 주거 구역",
      "floor_distribution": { "2": 0.20, "3": 0.35, "4": 0.25, "5": 0.20 },
      "type_distribution": {
        "apartment": 0.45,
        "shop_ground": 0.20,
        "garage": 0.15,
        "courtyard_wall": 0.20
      },
      "footprint_m2_range": [50, 250],
      "spacing_m": 6,
      "color_palette": "worn_plaster"
    }
  }
}
```

---

## 4. BR 생성 파이프라인

BR 맵은 대규모 (4~8km) 탑뷰 이미지에서 다음을 추출/생성합니다.

### 4.1 Step 1: 이미지 분석

| 단계 | 입력 | 출력 | 방법 |
|------|------|------|------|
| **육지/수역 분리** | map_clean.png | 바이너리 마스크 | HSV 색상 분류 — 바다(H:200-220, S>40, V<40) 분리 |
| **내륙 수역 검출** | map_clean.png | 강/호수 마스크 | 청록색 (H:160-200) 별도 추출 |
| **Heightmap 추출** | 육지 마스크 영역 | 고도 그리드 | 그레이스케일 밝기 → 고도 매핑. 어두움=높음, 밝음=낮음 |
| **도로 추출** | map_clean.png | 라인 세그먼트 배열 | 고밝기 픽셀 (>200) threshold → 세선화 → 라인 추적 |

### 4.2 Step 2: 메타데이터 병합

| 단계 | 입력 | 출력 | 방법 |
|------|------|------|------|
| **POI 배치 계획** | pois.json + 이미지 크기 | POI별 월드 좌표 + 반경 | `center_pct × map_size_km × 100000` |
| **Heightmap 보정** | heightmap + POI elevation_hint | 보정된 heightmap | `hilltop` → 반경 내 고도 ↑, `valley` → ↓, `coastal` → 해수면 근접 |
| **도로 보정** | 추출 도로 + POI connections | 정리된 도로 네트워크 | POI 간 연결 누락 시 직선 도로 추가, 가짜 도로 제거 |
| **다리 생성** | island POI + bridge_connections | 다리 세그먼트 | 섬 POI의 `bridge_connections`에서 본토 연결점 계산 |

### 4.3 Step 3: UE5 월드 생성

| 단계 | 생성물 | 방법 |
|------|--------|------|
| **Landscape** | ALandscapeProxy | heightmap → Landscape Import API |
| **수역** | Water Body Actor 또는 평면 메시 | 수역 마스크 → 폴리곤 변환 → 배치 |
| **도로** | ProceduralMeshComponent | 라인 세그먼트 → 폭 적용 → 메시 생성 |
| **건물** | StaticMeshActor (Cube 기반) | POI별: style → floor/type 분포에서 샘플링 → 반경 내 배치 |
| **랜드마크** | 특수 메시 또는 태그된 Actor | `landmarks` 배열에서 POI 중심에 배치 |
| **POI간 분산 건물** | 소규모 건물 클러스터 | 이미지 밝기 기반 밀집도 분석 → POI 외곽에 산포 |

### 4.4 BR 생성 규칙

| 규칙 | 설명 | 근거 |
|------|------|------|
| **섬 분리** | `island: true` POI는 수역으로 분리. 다리만 연결 | Erangel Military Island |
| **S등급 간 최소 거리** | S등급 POI 간 1.5km 이상 이격 확인 (경고만) | Erangel 설계 원칙 |
| **도로 커버리지** | 추출 도로가 map_meta의 기대 비율 미만 시 자동 보충 | Erangel 4.28% |
| **고도 범위** | map_meta.elevation_range_m 내로 클램핑 | 지형 사실성 |
| **해안선 부드러움** | 마스크 경계에 가우시안 블러 적용 | 자연스러운 해안선 |
| **건물-도로 오프셋** | 건물은 도로에서 최소 5m 이격 | 겹침 방지 |
| **건물-수역 오프셋** | 건물은 수역에서 최소 20m 이격 | 수역 배치 방지 |

---

## 5. EX 생성 파이프라인

EX(Extraction) 맵은 BR과 근본적으로 다른 규칙이 필요합니다.

### 5.1 BR과 EX의 핵심 차이

| 항목 | BR | EX |
|------|----|----|
| 스케일 | 4~8 km | 0.5~2 km |
| 핵심 요소 | POI 분포, 도로망, 자기장 공정성 | 동선 병목, 추출 포인트, 실내 교전 |
| 건물 역할 | 루트 장소 / 엄폐물 | 게임플레이 그 자체 |
| 이미지 활용도 | 높음 (탑뷰로 대부분 파악) | 중간 (외관만, 실내 불가) |
| 메타 의존도 | 중간 | 높음 (동선, 추출, 열쇠 등) |

### 5.2 EX 전용 메타데이터 — routes.json

```json
{
  "version": "1.0",
  "extraction_points": [
    {
      "name": "Crossroads",
      "location_pct": [0.15, 0.50],
      "type": "always_open",
      "exposure": "exposed",
      "notes": "개방 지역. 접근 시 노출"
    },
    {
      "name": "ZB-1011",
      "location_pct": [0.80, 0.30],
      "type": "conditional",
      "condition": "key_required",
      "exposure": "shielded",
      "notes": "지하 벙커. 열쇠 필요"
    }
  ],
  "spawn_zones": [
    { "name": "Big Red Side", "area_pct": [[0.0, 0.0], [0.3, 1.0]] },
    { "name": "Trailer Park Side", "area_pct": [[0.7, 0.0], [1.0, 1.0]] }
  ],
  "key_routes": [
    {
      "from": "Big Red",
      "to": "Dorms",
      "type": "main_road",
      "risk": "high",
      "choke_points": ["Construction", "Gas Station"]
    }
  ],
  "vertical_layers": [
    { "name": "underground", "depth_m": -5, "access": ["Dorms_basement", "ZB-bunkers"] },
    { "name": "ground", "depth_m": 0 },
    { "name": "upper", "depth_m": 8, "access": ["Dorms_3floor", "Factory_roof"] }
  ]
}
```

### 5.3 EX 생성 규칙

| 규칙 | 설명 | 근거 |
|------|------|------|
| **추출 포인트 배치** | routes.json의 extraction_points를 먼저 배치 | EX의 핵심 목표 |
| **스폰→추출 동선** | 스폰 존에서 추출 포인트까지 최소 2개 경로 보장 | 동선 다양성 |
| **병목 생성** | key_routes의 choke_points에 좁은 통로/건물 벽 배치 | 긴장감 유지 |
| **건물 밀도 높음** | BR 대비 2~3배 밀집 (spacing_m 감소) | 실내 교전 비중 |
| **수직 레이어** | vertical_layers에 따라 지하/지상/상층 구분 생성 | EX 수직 전투 |
| **노출 구간 200m 이하** | POI 간 연속 노출 구간이 200m를 넘지 않도록 중간에 엄폐물 배치 | EX-4 은신 루트 원칙 |
| **추출 긴장감** | exposure: "exposed" 추출 포인트 주변은 건물 1개 이하 | 추출 시 긴장감 |

---

## 6. 내장 레퍼런스 — Battle Royale

초기 내장 맵 목록:

| 맵 | 게임 | 크기 | 테마 | 우선순위 |
|----|------|------|------|----------|
| **Erangel** | PUBG | 8×8km | 동유럽 섬 | **1순위 (구현 대상)** |
| Miramar | PUBG | 8×8km | 사막 | 2순위 |
| Sanhok | PUBG | 4×4km | 정글 | 3순위 |
| Verdansk | Warzone | ~6km | 도시 | 4순위 |
| Taego | PUBG | 8×8km | 한국 80년대 | 5순위 |

각 맵의 상세 분석은 [13. 레퍼런스 맵 상세 데이터](#13-레퍼런스-맵-상세-데이터) 참조.

---

## 7. 내장 레퍼런스 — Extraction

| 맵 | 게임 | 크기 | 테마 | 우선순위 |
|----|------|------|------|----------|
| **Customs** | Tarkov | ~1.5km | 산업/관세 | 1순위 |
| Reserve | Tarkov | ~1km | 군사 지하 | 2순위 |
| Dam Battlegrounds | ARC Raiders | ~2.5km | 댐/수직 | 3순위 |
| Buried City | ARC Raiders | Large | 매몰 도시 | 4순위 |
| Lawson Delta | Hunt: Showdown | ~1km | 늪지 | 5순위 |

---

## 8. 건물 스타일 프로파일

스타일 프로파일은 `building_styles.json`에 정의되며, POI의 `style` 필드로 참조됩니다.

### 생성 로직

```
1. POI의 style → building_styles.json에서 프로파일 로드
2. building_count만큼 반복:
   a. floor_distribution에서 층수 샘플링 (확률 분포)
   b. type_distribution에서 건물 유형 샘플링
   c. footprint_m2_range에서 면적 랜덤 결정
   d. POI 중심에서 반경 내 랜덤 위치 (spacing_m 이격 보장)
   e. 높이 = 층수 × 3m
   f. 유형별 종횡비 적용 (warehouse=2:1, house=1:1, church=0.5:1+첨탑)
3. landmarks 배열의 특수 건물은 POI 중심 근처에 우선 배치
```

### 확장 방법

기획자가 `building_styles.json`에 새 스타일을 추가하면 모든 POI에서 즉시 사용 가능:

```json
"korean_80s_rural": {
  "label": "한국 80년대 농촌",
  "floor_distribution": { "1": 0.60, "2": 0.30, "3": 0.10 },
  "type_distribution": {
    "hanok": 0.20,
    "modern_house": 0.30,
    "market_stall": 0.15,
    "school": 0.05,
    "rice_paddy_hut": 0.15,
    "bathhouse": 0.05,
    "gas_station": 0.10
  },
  "footprint_m2_range": [30, 120],
  "spacing_m": 12,
  "color_palette": "tile_roof_cream_wall"
}
```

---

## 9. 이미지 분석 엔진 (UE5 C++)

UE5 내장 이미지 처리로 구현합니다. 외부 라이브러리 의존 없음.

### 9.1 핵심 클래스

| 클래스 | 역할 |
|--------|------|
| `UMapImageAnalyzer` | 이미지 로드 → 마스크/heightmap/도로 추출 |
| `URefMapGenerator` | 분석 결과 + 메타데이터 → UE5 월드 생성 |
| `URefMapDataManager` | 레퍼런스 데이터 (JSON/이미지) 로드/관리 |
| `SRefMapPanel` | 에디터 Slate UI |

### 9.2 이미지 분석 알고리즘

#### 육지/수역 분리

```
입력: RGBA 이미지
1. 각 픽셀을 HSV로 변환
2. 바다 판정: H ∈ [200, 220] && S > 0.4 && V < 0.4
3. 내륙 수역: H ∈ [160, 200] && S > 0.3
4. 나머지 = 육지
5. 육지 마스크에 모폴로지 클로징 (3px) → 노이즈 제거
6. 해안선 경계에 가우시안 블러 (σ=2px) → 부드러운 경계
출력: 바이너리 마스크 (land/sea) + 내륙수역 마스크
```

#### Heightmap 추출

```
입력: 육지 마스크 내 RGBA 이미지
1. 그레이스케일 변환: Y = 0.299R + 0.587G + 0.114B
2. 도로 픽셀 제외 (밝기 > 200 이면서 주변에 도로 라인 존재)
3. 밝기 반전: 어두움 = 높음 (산/언덕), 밝음 = 낮음 (평지)
4. min-max 정규화 → [0, 1]
5. elevation_range_m 매핑: height = min + normalized × (max - min)
6. POI elevation_hint 보정:
   - hilltop: 반경 내 가우시안 범프 추가
   - valley: 반경 내 가우시안 딥 추가
   - coastal: 해안선 방향으로 경사 강제
   - flat: 반경 내 평탄화
7. 수역 경계: 해수면(0m)으로 연속 경사 보간
출력: float 그리드 (미터 단위 고도)
```

#### 도로 추출

```
입력: RGBA 이미지
1. 그레이스케일 변환
2. threshold (밝기 > 200) → 바이너리
3. 수역 마스크 영역 제거
4. 모폴로지 오프닝 (1px) → 텍스트 잔여 노이즈 제거
5. 세선화 (Zhang-Suen thinning) → 1px 폭 스켈레톤
6. 라인 추적: 연결된 픽셀을 폴리라인으로 그룹핑
7. 짧은 세그먼트 (< 20px) 제거 → 노이즈 필터
8. Douglas-Peucker 단순화 (ε = 3px)
9. POI connections 메타와 교차 검증:
   - 메타에 있는 연결인데 도로가 없으면 → 직선 도로 추가
   - 메타에 없는 고립된 짧은 선분 → 가짜 도로로 제거
출력: 라인 세그먼트 배열 (UE5 월드 좌표)
```

### 9.3 UE5 API 활용

| 기능 | UE5 API |
|------|---------|
| 이미지 로드 | `IImageWrapperModule`, `FImage` |
| 픽셀 접근 | `FColor*` 직접 접근 |
| HSV 변환 | `FLinearColor::LinearRGBToHSV()` |
| Landscape 생성 | `ALandscapeProxy`, `ULandscapeInfo`, Heightmap Import |
| 메시 생성 | `UProceduralMeshComponent` (도로) |
| Actor 스폰 | `UWorld::SpawnActor<AStaticMeshActor>()` |
| JSON 파싱 | `FJsonSerializer`, `FJsonObject` |

---

## 10. 에디터 UI

### 10.1 UI 구조

```
LevelTool 탭
├── [기존] 좌표 기반 생성 (Stage 1)
└── [신규] 레퍼런스 맵 생성
    ├── 카테고리 선택: [Battle Royale] [Extraction]
    ├── 맵 선택: [Erangel ▼] (썸네일 프리뷰)
    ├── 설정
    │   ├── 생성 스케일: [1:1] [1:2 축소] [커스텀]
    │   └── 옵션: ☑ Landscape  ☑ Buildings  ☑ Roads  ☑ Water
    ├── 메타데이터 편집: [pois.json 열기] [styles.json 열기]
    └── [▶ 맵 생성] 버튼
```

### 10.2 생성 진행률 표시

```
[▶ 맵 생성] 클릭 후:
  [████████░░░░░░░░] 50%  이미지 분석 중...
  [████████████░░░░] 75%  건물 배치 중... (142/210)
  [████████████████] 100% 완료! (Landscape + 210 Buildings + 45 Roads + Water)
```

### 10.3 생성 후 뷰

생성 완료 후:
- 뷰포트가 맵 전체를 조감하는 위치로 자동 이동
- Outliner에 `LevelTool/RefMap_Erangel/` 폴더 아래 정리
  - `Landscape_Erangel`
  - `Buildings/` (POI별 서브폴더)
  - `Roads/`
  - `Water/`

---

## 11. 기존 시스템과의 관계

### 삭제 대상 (2~3단계 코드)

| 파일 | 내용 | 처리 |
|------|------|------|
| `DesignerIntentSubsystem.h/.cpp` | 슬라이더/프리셋/유효상태 관리 | 삭제 |
| `SliderLayerGenerator.h/.cpp` | 슬라이더→레이어 생성 | 삭제 |
| `PresetManager.h/.cpp` | 프리셋 관리 | 삭제 |
| `ChecklistEngine.h/.cpp` | BR/EX 진단 체크리스트 | 삭제 |
| `HeatmapGenerator.h/.cpp` | 히트맵 데이터/오버레이 | 삭제 |
| `SDesignerIntentPanel.h/.cpp` | 2~3단계 Slate UI | 삭제 |
| `EditLayerManager.h/.cpp` | Edit Layer CRUD | **유지** — 새 시스템에서도 활용 |
| `EditLayerApplicator.h/.cpp` | Layer→Actor 반영 | **유지** — 건물/도로 배치에 활용 |
| `EditLayerTypes.h` | FEditLayer 등 타입 | **유지** |

### 유지 대상 (1단계 + 공통)

| 파일 | 내용 | 비고 |
|------|------|------|
| `LevelToolSubsystem.h/.cpp` | OSM 기반 생성 (1단계) | 그대로 유지 |
| `LevelToolSubsystem_Landscape/Buildings/Roads/Water` | 1단계 생성 로직 | 그대로 유지 |
| `LevelToolModule.h/.cpp` | 플러그인 등록 | 탭 추가 |
| `LevelToolValidator.h/.cpp` | 검증 | 유지 (새 시스템 검증에도 활용) |

---

## 12. 구현 순서

### Phase 1: 핵심 파이프라인 — 에란겔 BR (4~5 스프린트)

| 순서 | 작업 | 산출물 |
|------|------|--------|
| **S1** | 레퍼런스 데이터 구조 + 에란겔 JSON 작성 | map_meta.json, pois.json, building_styles.json |
| **S2** | 클린 이미지 확보 + `UMapImageAnalyzer` (육지/수역 마스크) | map_clean.png, 마스크 추출 |
| **S3** | Heightmap 추출 + POI 고도 보정 | heightmap 그리드 |
| **S4** | 도로 추출 + 메타 보정 | 도로 네트워크 |
| **S5** | `URefMapGenerator` — Landscape + 수역 + 도로 + 건물 배치 | 에란겔 생성 가능 |

### Phase 2: UI + 폴리시 (2 스프린트)

| 순서 | 작업 | 산출물 |
|------|------|--------|
| **S6** | `SRefMapPanel` 에디터 UI + 진행률 | 맵 선택 → 원클릭 생성 |
| **S7** | 2~3단계 코드 삭제 + 정리 | 깔끔한 코드베이스 |

### Phase 3: EX + 추가 맵 (3+ 스프린트)

| 순서 | 작업 | 산출물 |
|------|------|--------|
| **S8** | EX 파이프라인 (routes.json, 추출 포인트, 병목) | Customs 생성 가능 |
| **S9** | 추가 BR 맵 (Miramar, Sanhok) | 데이터 추가 |
| **S10** | 추가 EX 맵 (Reserve, Dam) | 데이터 추가 |

---

## 13. 레퍼런스 맵 상세 데이터

> 이전 문서(v4.4)의 레퍼런스 맵 분석을 그대로 승계합니다.  
> 아래는 각 맵의 생성 시 활용할 핵심 수치 요약입니다.

### BR 맵 비교 요약

| 항목 | Erangel | Miramar | Sanhok | Vikendi | Taego | Verdansk |
|------|---------|---------|--------|---------|-------|----------|
| 크기 | 8×8 km | 8×8 km | 4×4 km | 6×6 km | 8×8 km | ~6 km |
| 육지 | 51.47% | 80.59% | 49.26% | 40.29% | ~60% | ~70% |
| 플레이어 | 100 | 100 | 100 | 100 | 100 | 150 |
| 건물 밀도 | 중간 | 낮음 | 높음 | 중간 | 높음 | 매우 높음 |
| 도로 | 4.28% | 6.92% | 7.27% | — | 높음 | 높음 |
| 식생 | 30.64% | 32.64% | 43.18% | 6.92% | ~30% | 낮음 |
| 엄폐 주력 | 건물+나무 | 능선 | 식생 | 건물+바위 | 건물+논 | 건물+차량 |
| 고유 병목 | 다리 2개 | 능선/협곡 | 섬 간 다리 | 산맥 | 강 | 지하터널 |
| 테마 | 동유럽 섬 | 멕시코 사막 | 동남아 정글 | 북유럽 설원 | 한국 80년대 | 동유럽 도시 |
| 고도 범위 | 0~200m | 0~300m | 0~150m | 0~250m | 0~180m | 0~150m |
| POI (S급) | 3 | 2 | 2 | 3 | 3 | 3 |
| POI (총) | ~20 | ~18 | ~15 | ~18 | ~20 | ~19 |

### EX 맵 비교 요약

| 항목 | Customs | Reserve | Dam (ARC) | Buried City (ARC) | Lawson Delta (Hunt) |
|------|---------|---------|-----------|-------------------|---------------------|
| 크기 | ~1.5km | ~1km | ~2.5km | Large | ~1km |
| 추출 포인트 | 6~8 | 6~8 | 6~7 | 8 | 3 |
| 수직 레이어 | 2 (지상+지하) | 3 (지상+지하2층) | 4단 | 2 | 1 |
| 핵심 병목 | 건널목, Dorms | 열차역, 지하 | 댐 상단 | 메트로 입구 | 다리/늪지 |
| 건물 밀도 | 매우 높음 | 높음 | 높음 | 매우 높음 | 중간 |
| 실내 비중 | 70%+ | 60%+ | 50% | 70%+ | 30% |
| 테마 | 산업/관세 | 군사 지하 | 수력 발전 | 매몰 도시 | 미국 남부 늪지 |

---

### 에란겔 상세 거점 데이터 (pois.json 기반 확장 정보)

| 거점 | 위치(%) | 반경(m) | 건물 수 | 평균 층수 | 스타일 | 루트 등급 | 고도 힌트 |
|------|---------|---------|---------|-----------|--------|-----------|-----------|
| Sosnovka Military Base | 52, 85 | 500 | 30 | 1 | military | S | flat (섬) |
| Pochinki | 42, 52 | 300 | 42 | 2 | eastern_rural | S | flat |
| School | 47, 38 | 100 | 5 | 3 | institutional | S | flat |
| Georgopol | 18, 35 | 400 | 80 | 3 | port_industrial | A | coastal |
| Yasnaya Polyana | 72, 28 | 350 | 60 | 3 | dense_urban | A | gentle_hill |
| Mylta Power | 85, 65 | 150 | 15 | 2 | power_plant | A | coastal |
| Novorepnoye | 88, 82 | 200 | 35 | 2 | port_industrial | A | coastal |
| Rozhok | 45, 35 | 200 | 25 | 2 | eastern_rural | B | gentle_hill |
| Prison | 65, 60 | 120 | 8 | 2 | institutional | B | hilltop |
| Mansion | 72, 52 | 80 | 3 | 2 | eastern_rural | B | gentle_hill |
| Primorsk | 18, 78 | 200 | 20 | 2 | eastern_rural | B | coastal |
| Ferry Pier | 35, 82 | 100 | 8 | 1 | port_industrial | B | coastal |
| Hospital | 13, 48 | 100 | 4 | 3 | institutional | B | flat |
| Shooting Range | 35, 15 | 100 | 8 | 1 | military | B | gentle_hill |
| Stalber | 72, 12 | 80 | 5 | 1 | military_outpost | C | hilltop |
| Zharki | 8, 8 | 150 | 12 | 1 | eastern_rural | C | coastal |
| Severny | 30, 8 | 150 | 15 | 1 | eastern_rural | C | coastal |
| Gatka | 25, 55 | 150 | 12 | 1 | eastern_rural | C | flat |
| Quarry | 18, 68 | 120 | 5 | 1 | quarry | C | valley |
| Lipovka | 80, 42 | 150 | 15 | 1 | eastern_rural | C | coastal |
| Kameshki | 92, 10 | 120 | 10 | 1 | eastern_rural | C | coastal |

---

*끝*
