# LevelTool — Reference Map Generation 기획문서 v1.2

> **이전 문서**: `LevelTool_DesignerIntent_보완문서.md` v4.4 (2~3단계 슬라이더/진단 기반 → **폐기**)  
> **변경 사유**: 슬라이더/프리셋/진단 시스템을 레퍼런스 게임 맵 이미지 기반 직접 생성으로 전면 교체  
> **핵심 변경**: "추상적 수치 조작" → "레퍼런스 맵 복제 수준 생성"
>
> ### v1.2 변경 이력 (2026-03-20)
> - §4.3 POI간 분산 건물: 이미지 밝기 기반 → **도로변 메타데이터 기반 산포**로 수정
> - §4.1 이미지 분석 **Fallback 절차** 추가 (이미지 없이 land_ratio 기반 프로시저럴 섬 생성)
> - §4.3 **Landscape 해상도** 테이블 추가 (맵 크기별 vertex 간격·그리드 크기·UE5 컴포넌트 수)
> - §4.3 건물 메시: Cube → **절차적 벽+지붕** 생성으로 품질 향상 (유형별 Ridge/Flat/Spire 지붕)
> - §12 **스프린트 기간(2주) + 인력(1인) + 총 예상 기간(14주)** 명시
>
> ### v1.1 변경 이력 (2026-03-20)
> - 이미지 분석 범위를 **해안선/마스크 추출에 한정** (heightmap·도로 추출 삭제)
> - Heightmap을 **메타데이터 주도 절차 생성**으로 전환 (이미지 밝기 기반 제거)
> - `roads_hint.json`을 **필수** 데이터로 격상
> - 구현 순서에서 **기존 Stage 2-3 코드 삭제를 S0으로 최우선 배치**
> - **검증 기준(합격 조건)** 섹션 신설 (§14)

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
14. [검증 기준 (합격 조건)](#14-검증-기준-합격-조건)

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
│   │   ├── map_clean.png      ← 텍스트 없는 탑뷰 이미지 (해안선/마스크 추출용)
│   │   ├── map_meta.json      ← 맵 크기, 육지 비율, 기본 설정
│   │   ├── pois.json          ← POI 목록 (위치, 유형, 등급, 건물 수, 고도 힌트)
│   │   └── roads_hint.json    ← **[필수]** 주요 도로 좌표 배열
│   ├── Miramar/ ...
│   └── Verdansk/ ...
├── EX/
│   ├── Customs/
│   │   ├── map_clean.png
│   │   ├── map_meta.json
│   │   ├── pois.json
│   │   ├── roads_hint.json    ← **[필수]** 주요 도로 좌표 배열
│   │   └── routes.json        ← 추출 포인트, 핵심 동선
│   └── Reserve/ ...
└── Styles/
    └── building_styles.json   ← 건물 스타일 프로파일 (공용)
```

### 생성 플로우

```
[사용자: 맵 선택 (예: Erangel)] → [버튼: 생성]
    │
    ├── 1. 이미지 분석 (UE5 C++) — 해안선/마스크 전용
    │   ├── 육지/수역 마스크 추출 (HSV 색상 분류)
    │   └── 내륙 수역 마스크 추출 (강/호수)
    │   ※ heightmap, 도로는 이미지에서 추출하지 않음
    │
    ├── 2. 메타데이터 주도 생성
    │   ├── POI 위치/크기/유형 → 건물 배치 계획
    │   ├── POI elevation_m + elevation_hint → heightmap 절차 생성
    │   ├── roads_hint.json → 도로 네트워크 (필수 데이터)
    │   └── 스타일 프로파일 → 건물 높이/유형 결정
    │
    └── 3. UE5 월드 생성
        ├── Landscape 생성 (절차 생성 heightmap 적용)
        ├── 수역 생성 (이미지 마스크 기반)
        ├── 도로 생성 (roads_hint.json 좌표 기반)
        └── 건물 배치 (POI별 스타일 기반 절차적 배치)
```

### 이미지 vs 메타데이터 역할 분담

| 데이터 | 소스 | 근거 |
|--------|------|------|
| **해안선/섬 형태** | 이미지 분석 | 색상 차이가 뚜렷하여 신뢰성 높음 |
| **내륙 수역** | 이미지 분석 | 청록색 계열 별도 추출 가능 |
| **Heightmap** | 메타데이터 절차 생성 | 이미지 밝기 ≠ 고도 (식생/그림자 영향). POI elevation_hint로 충분 |
| **도로** | roads_hint.json (필수) | 이미지 도로 추출은 노이즈 과다. 좌표 기입이 정확 |
| **건물 배치** | pois.json + building_styles.json | 이미지 해상도로는 개별 건물 식별 불가 |

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

### 3.3 roads_hint.json (필수)

이미지에서의 도로 추출은 노이즈가 과다하여, 도로 데이터는 **직접 좌표로 기입**합니다.

```json
{
  "version": "1.0",
  "roads": [
    {
      "name": "Pochinki-Rozhok Main Road",
      "type": "main_road",
      "width_m": 12,
      "points_pct": [
        [0.42, 0.52],
        [0.43, 0.48],
        [0.44, 0.42],
        [0.45, 0.35]
      ]
    },
    {
      "name": "Georgopol-Hospital Road",
      "type": "main_road",
      "width_m": 10,
      "points_pct": [
        [0.18, 0.35],
        [0.15, 0.40],
        [0.13, 0.48]
      ]
    },
    {
      "name": "South Bridge",
      "type": "bridge",
      "width_m": 8,
      "points_pct": [
        [0.42, 0.71],
        [0.42, 0.79]
      ]
    },
    {
      "name": "East Bridge",
      "type": "bridge",
      "width_m": 8,
      "points_pct": [
        [0.58, 0.71],
        [0.60, 0.79]
      ]
    }
  ],
  "road_types": {
    "main_road": { "width_m": 10, "surface": "asphalt", "has_shoulder": true },
    "secondary":  { "width_m": 6,  "surface": "asphalt", "has_shoulder": false },
    "dirt_road":  { "width_m": 4,  "surface": "dirt",    "has_shoulder": false },
    "bridge":     { "width_m": 8,  "surface": "concrete","has_shoulder": true }
  }
}
```

> **`points_pct`**: 0~1 비율 좌표 배열. 순서대로 연결하여 폴리라인 생성.  
> **POI connections와 교차 검증**: pois.json의 `connections`에 있는 연결인데 roads에 경로가 없으면 경고 출력.

### 3.4 building_styles.json

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

### 4.1 Step 1: 이미지 분석 (해안선/마스크 전용)

이미지 분석의 역할은 **해안선과 수역 형태 추출에 한정**합니다.  
Heightmap과 도로는 이미지 분석의 신뢰성이 낮아 메타데이터로 대체합니다.

| 단계 | 입력 | 출력 | 방법 |
|------|------|------|------|
| **육지/수역 분리** | map_clean.png | 바이너리 마스크 | HSV 색상 분류 — 바다(H:200-220, S>40, V<40) 분리 |
| **내륙 수역 검출** | map_clean.png | 강/호수 마스크 | 청록색 (H:160-200) 별도 추출 |
| ~~Heightmap 추출~~ | — | — | **삭제** — 이미지 밝기 ≠ 고도 (식생/그림자 영향). §4.2에서 절차 생성 |
| ~~도로 추출~~ | — | — | **삭제** — 노이즈 과다. roads_hint.json으로 대체 |

> **설계 판단 근거**: 게임 맵 탑뷰 이미지는 "스타일화된 일러스트"이며 고도 데이터가 아닙니다.  
> 이미지에서 신뢰성 있게 추출 가능한 것은 색상 차이가 뚜렷한 육지/수역 경계뿐입니다.

#### Fallback: 이미지 없이 생성

`map_clean.png`가 없거나 마스크 추출 품질이 낮을 경우 자동으로 fallback합니다.

```
Fallback 조건: 이미지 파일 미존재 OR 추출된 육지 비율이 map_meta.land_ratio와 30%p 이상 차이

Fallback 절차:
1. map_meta.land_ratio (예: 0.5147)를 기반으로 프로시저럴 섬 형태 생성
2. 맵 중앙에 Perlin Noise 기반 불규칙 blob 배치
   - blob 면적 = map_size² × land_ratio
   - 경계에 Perlin Noise (주파수 4~6) 적용 → 자연스러운 해안선
3. island: true인 POI가 있으면 해당 center_pct 위치에 별도 작은 섬 생성
4. 나머지 파이프라인(§4.2 Step 2, §4.3 Step 3)은 동일하게 진행

결과: 이미지 없이도 "대략적인 섬 형태 + 정확한 POI 배치"가 가능
```

> S2(메타데이터만으로 생성)에서 이 fallback이 기본 동작입니다.  
> S3에서 이미지를 추가하면 해안선이 실제 맵과 더 유사해집니다.

### 4.2 Step 2: 메타데이터 주도 생성

| 단계 | 입력 | 출력 | 방법 |
|------|------|------|------|
| **POI 배치 계획** | pois.json + 이미지 크기 | POI별 월드 좌표 + 반경 | `center_pct × map_size_km × 100000` |
| **Heightmap 절차 생성** | map_meta.elevation_range_m + POI elevation_m/hint | 고도 그리드 | 아래 알고리즘 참조 |
| **도로 네트워크** | roads_hint.json (필수) | 도로 세그먼트 배열 | 좌표 배열 직접 사용 + POI connections 교차 검증 |
| **다리 생성** | island POI + bridge_connections | 다리 세그먼트 | 섬 POI의 `bridge_connections`에서 본토 연결점 계산 |

#### Heightmap 절차 생성 알고리즘

이미지 대신 메타데이터만으로 heightmap을 생성합니다.

```
입력: map_meta.elevation_range_m, pois.json의 각 POI (elevation_m, elevation_hint, radius_m)

1. 기본 지형 생성
   a. 육지 마스크(이미지 분석 결과) 내에서 Perlin Noise 기반 base heightmap 생성
   b. 노이즈 주파수 3~4 옥타브 중첩 → 자연스러운 기복
   c. min-max를 elevation_range_m으로 정규화

2. POI 고도 적용 (elevation_hint별)
   a. hilltop: POI 중심에 가우시안 범프 추가 (+elevation_m)
   b. valley:  POI 중심에 가우시안 딥 추가 (-깊이)
   c. coastal: 해안선 방향으로 경사 강제 (elevation_m → 0m)
   d. flat:    반경 내 고도를 elevation_m으로 평탄화
   e. gentle_hill: 완만한 가우시안 범프 (hilltop의 50% 강도)

3. 해안선 경사 처리
   a. 육지 마스크 경계에서 안쪽으로 거리 계산
   b. 경계 200m 이내: 고도 × (거리/200m) → 해수면으로 부드럽게 연결
   c. 가우시안 블러 (σ=2) 적용 → 자연스러운 해안 경사

4. 클램핑
   a. 전체 heightmap을 elevation_range_m [min, max]로 클램핑
   b. 수역 영역은 0m (해수면)

출력: float 그리드 (미터 단위 고도)
```

> **기존 이미지 기반 대비 장점**: POI 고도가 정확하게 반영됨. 이미지 밝기의 식생/그림자 오염 없음.  
> **한계**: 실제 게임과 동일한 미세 지형은 재현 불가. 그러나 "알아볼 수 있는 수준"(산/평지/해안 구분)은 충분.

### 4.3 Step 3: UE5 월드 생성

| 단계 | 생성물 | 방법 |
|------|--------|------|
| **Landscape** | ALandscapeProxy | heightmap → Landscape Import API (아래 해상도 참조) |

#### Landscape 해상도 설정

| 맵 크기 | heightmap 해상도 | vertex 간격 | 총 vertex 수 | UE5 권장 컴포넌트 |
|---------|-----------------|-------------|-------------|-------------------|
| 8×8 km | 4033×4033 | ~2m | ~16.3M | 63×63 Components, 각 63×63 Quads |
| 4×4 km | 2017×2017 | ~2m | ~4.1M | 31×31 Components |
| 1.5 km (EX) | 505×505 | ~3m | ~255K | 8×8 Components |

> UE5 Landscape는 `(N×ComponentSize + 1)` 해상도를 요구합니다 (N=컴포넌트 수, ComponentSize=63 권장).  
> 2m 간격은 POI 반경(80~500m) 내 지형 변화를 충분히 표현하면서도 에디터 성능을 유지합니다.  
> 생성 스케일이 1:2 축소인 경우 vertex 간격을 2배로 늘려 성능 확보.
| **수역** | Water Body Actor 또는 평면 메시 | 수역 마스크 → 폴리곤 변환 → 배치 |
| **도로** | ProceduralMeshComponent | roads_hint.json 좌표 → 폭 적용 → 메시 생성 |
| **건물** | StaticMeshActor (절차적 벽+지붕) | POI별: style → floor/type 분포에서 샘플링 → 반경 내 배치 (아래 메시 생성 참조) |

#### 건물 메시 생성 방식

단순 Cube가 아닌, 최소한 **벽+지붕**이 구분되는 절차적 메시를 생성합니다.

```
건물 메시 생성 (유형별):
┌─────────────────────────────────────────────────────┐
│ [공통] 벽체: footprint → Extrude (높이 = 층수 × 3m)  │
│         UV: 벽면 = 4m당 1타일, 지붕 = 8m당 1타일     │
├─────────────────────────────────────────────────────┤
│ house/shop/apartment:                                │
│   벽체 + 경사 지붕 (Ridge Roof, 30° 경사)            │
│   → 상단 2개 삼각형 면 추가                          │
│                                                      │
│ warehouse/hangar/factory_hall:                        │
│   벽체 + 평지붕 (Flat Roof)                          │
│   → 종횡비 2:1 이상                                  │
│                                                      │
│ church/watchtower:                                    │
│   벽체 + 첨탑 (Spire)                                │
│   → 중앙에 좁은 원기둥 + 원뿔 추가 (높이 × 1.5)      │
│                                                      │
│ barracks/bunker:                                      │
│   벽체 + 평지붕, 낮은 높이 (1~2층)                    │
└─────────────────────────────────────────────────────┘

구현 방식:
  Option A: UProceduralMeshComponent — 런타임 절차 생성 (기본)
  Option B: blender_extrude.py 재활용 — JSON→FBX 사전 생성 후 Import (고품질)
```

> Option A(절차 생성)로 시작하고, 품질이 부족하면 Option B로 전환합니다.  
> Option A만으로도 "벽+지붕 구분"이 되면 탑뷰에서 건물 식별이 가능합니다.
| **랜드마크** | 특수 메시 또는 태그된 Actor | `landmarks` 배열에서 POI 중심에 배치 |
| **POI간 분산 건물** | 소규모 건물 클러스터 | POI connections 도로 경로를 따라 도로변에 소규모 건물 산포 (간격 = spacing_m × 3) |

### 4.4 BR 생성 규칙

| 규칙 | 설명 | 근거 |
|------|------|------|
| **섬 분리** | `island: true` POI는 수역으로 분리. 다리만 연결 | Erangel Military Island |
| **S등급 간 최소 거리** | S등급 POI 간 1.5km 이상 이격 확인 (경고만) | Erangel 설계 원칙 |
| **도로 커버리지** | roads_hint.json 기반. POI connections에 도로 누락 시 경고 | Erangel 4.28% |
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
   f. 유형별 종횡비 적용 (warehouse=2:1, house=1:1, church=0.5:1)
   g. 유형별 지붕 적용:
      - house/shop/apartment → 경사 지붕 (Ridge, 30°)
      - warehouse/hangar/factory → 평지붕 (Flat)
      - church/watchtower → 첨탑 (Spire, 높이 × 1.5)
      - barracks/bunker → 평지붕 (낮은)
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
**역할은 해안선/수역 마스크 추출에 한정**합니다. Heightmap과 도로는 메타데이터로 처리.

### 9.1 핵심 클래스

| 클래스 | 역할 |
|--------|------|
| `UMapImageAnalyzer` | 이미지 로드 → **육지/수역 마스크 추출** (해안선 전용) |
| `URefMapGenerator` | 마스크 + 메타데이터 → UE5 월드 생성 (heightmap 절차생성 포함) |
| `URefMapDataManager` | 레퍼런스 데이터 (JSON/이미지) 로드/관리 |
| `SRefMapPanel` | 에디터 Slate UI |

### 9.2 이미지 분석 알고리즘 — 해안선/마스크 전용

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

> **맵별 HSV 튜닝**: 맵마다 색상 팔레트가 다르므로, `map_meta.json`에 선택적  
> `"water_hsv_override": {"h_min": 200, "h_max": 220, "s_min": 0.4}` 필드를 지원합니다.

#### ~~Heightmap 추출~~ (삭제)

> 이미지 밝기 ≠ 고도입니다. 숲은 어둡지만 높은 곳이 아니고, 모래는 밝지만 평지입니다.  
> **대체**: §4.2의 "Heightmap 절차 생성 알고리즘"으로 전환.

#### ~~도로 추출~~ (삭제)

> 이미지에서 도로(밝은 선)를 추출하면 건물 지붕, 해변, 텍스트 잔여물이 함께 감지됩니다.  
> **대체**: `roads_hint.json` (필수) 좌표 배열로 직접 기입.

### 9.3 UE5 API 활용

| 기능 | UE5 API |
|------|---------|
| 이미지 로드 | `IImageWrapperModule`, `FImage` |
| 픽셀 접근 | `FColor*` 직접 접근 |
| HSV 변환 | `FLinearColor::LinearRGBToHSV()` |
| Perlin Noise | `FMath::PerlinNoise3D()` (heightmap 절차 생성) |
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

### 전제 조건

| 항목 | 값 |
|------|-----|
| 스프린트 길이 | **2주** (10 영업일) |
| 인력 | **1인** (기획+구현 겸임, AI 코딩 도구 활용) |
| 총 예상 기간 | Phase 0~2: **14주** (S0~S6), Phase 3: +6주 (S7~S9) |
| 개발 환경 | UE5.7, C++, Slate UI |

> 1인 개발 + AI 도구 활용을 전제합니다.  
> 스프린트 내 구현 + 검증을 포함합니다. 검증 미통과 시 해당 스프린트를 1주 연장.

### Phase 0: 코드 정리 (1 스프린트)

> 기존 Stage 2-3 코드(~6,900줄)와 신규 코드가 공존하면 혼란이 발생합니다.  
> 깨끗한 코드베이스에서 시작합니다.

| 순서 | 작업 | 산출물 | 삭제 대상 |
|------|------|--------|-----------|
| **S0** | Stage 2-3 코드 전량 삭제 + 빌드 확인 | 클린 코드베이스 | DesignerIntentSubsystem, SliderLayerGenerator, PresetManager, ChecklistEngine, HeatmapGenerator, SDesignerIntentPanel + 관련 타입 헤더 |

### Phase 1: 핵심 파이프라인 — 에란겔 BR (4 스프린트)

| 순서 | 작업 | 산출물 | 검증 |
|------|------|--------|------|
| **S1** | 에란겔 JSON 작성 + `URefMapDataManager` 데이터 로더 | map_meta.json, pois.json, roads_hint.json, building_styles.json + 로더 | JSON 로드 → 로그 출력 확인 |
| **S2** | 메타데이터만으로 월드 생성 (heightmap 절차생성 + 건물 배치) | `URefMapGenerator` 코어 — Landscape + 건물 | **이미지 없이** POI 배치 + 지형 확인 |
| **S3** | 클린 이미지 확보 + `UMapImageAnalyzer` (해안선/마스크만) | map_clean.png, 마스크 추출 | 마스크 → Landscape 육지/수역 형태 일치 |
| **S4** | 도로 생성 (roads_hint.json) + 수역 생성 + 통합 | 에란겔 전체 생성 가능 | §14 합격 조건 체크 |

### Phase 2: UI + 폴리시 (2 스프린트)

| 순서 | 작업 | 산출물 |
|------|------|--------|
| **S5** | `SRefMapPanel` 에디터 UI + 진행률 표시 | 맵 선택 → 원클릭 생성 |
| **S6** | 생성 품질 폴리시 + 부분 재생성 지원 | POI 단위 재생성, 파라미터 미세 조정 |

### Phase 3: EX + 추가 맵 (3+ 스프린트)

| 순서 | 작업 | 산출물 |
|------|------|--------|
| **S7** | EX 파이프라인 (routes.json, 추출 포인트, 병목) | Customs 생성 가능 |
| **S8** | 추가 BR 맵 (Miramar, Sanhok) | 데이터 추가 |
| **S9** | 추가 EX 맵 (Reserve, Dam) | 데이터 추가 |

> **핵심 변경점**: S2에서 이미지 없이도 메타데이터만으로 월드 생성이 작동합니다.  
> 이미지 확보가 지연되어도 파이프라인이 멈추지 않습니다.

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

## 14. 검증 기준 (합격 조건)

### 14.1 에란겔 생성 합격 기준 (Phase 1 완료 시)

생성된 맵이 "에란겔을 알아볼 수 있는 수준"인지 판단하는 객관적 기준입니다.

#### 필수 조건 (전부 충족해야 합격)

| # | 기준 | 측정 방법 | 합격 값 |
|---|------|-----------|---------|
| V-1 | **섬 형태 인식** | 생성된 Landscape의 육지 마스크를 원본과 겹쳐 비교 (IoU) | IoU ≥ 0.60 |
| V-2 | **Military Island 분리** | 본섬과 분리된 별도 육지 영역 존재 + 다리 2개 연결 | 존재 여부 (boolean) |
| V-3 | **POI 위치 오차** | 각 POI 중심점의 기대 좌표 대비 오차 | 평균 오차 ≤ 200m |
| V-4 | **POI 개수** | 생성된 건물 클러스터 수 | 21개 POI 중 18개 이상 건물 존재 |
| V-5 | **건물 총 수** | 배치된 건물 Actor 수 | 400~550개 (기대값 460 ± 20%) |
| V-6 | **도로 연결성** | POI connections에 정의된 연결 중 도로가 존재하는 비율 | ≥ 85% |
| V-7 | **고도 범위** | Landscape의 min/max 고도 | 0~200m 이내 |
| V-8 | **S급 POI 간 거리** | S급 POI 쌍 간 최소 거리 | ≥ 1.2km |
| V-9 | **빌드 에러 없음** | UE5 에디터에서 빌드 성공 | 에러 0 |
| V-10 | **에디터 프레임** | 생성된 맵을 에디터에서 조감 시 FPS | ≥ 30 FPS |

#### 권장 조건 (품질 향상 지표)

| # | 기준 | 측정 방법 | 목표 값 |
|---|------|-----------|---------|
| V-11 | **고도 힌트 일치** | hilltop POI가 주변보다 실제로 높은 비율 | ≥ 80% |
| V-12 | **건물-수역 이격** | 건물이 수역에서 20m 이상 떨어진 비율 | ≥ 95% |
| V-13 | **건물-도로 이격** | 건물이 도로에서 5m 이상 떨어진 비율 | ≥ 90% |
| V-14 | **스타일 분포** | POI별 building_styles 확률 분포 대비 실제 건물 유형 편차 | χ² 편차 ≤ 0.15 |
| V-15 | **주관 평가** | 탑뷰 스크린샷을 보고 "에란겔인지 알아볼 수 있는가?" | 5명 중 4명 이상 인식 |

### 14.2 마일스톤별 검증

| 스프린트 | 검증 항목 | 기준 |
|----------|-----------|------|
| **S0** | 빌드 성공 + 기존 기능(Stage 1) 정상 작동 | 에러 0, OSM 생성 정상 |
| **S1** | JSON 로드 성공 + 데이터 무결성 | 21개 POI 파싱, 필수 필드 누락 0 |
| **S2** | 메타데이터만으로 Landscape + 건물 생성 | V-3, V-4, V-5, V-7 충족 |
| **S3** | 이미지 마스크 적용 시 섬 형태 개선 | V-1, V-2 충족 |
| **S4** | 에란겔 전체 생성 | V-1 ~ V-10 전부 충족 |
| **S5** | UI에서 원클릭 생성 | 버튼 클릭 → 3분 이내 완료 |

### 14.3 EX (Customs) 합격 기준 (Phase 3)

| # | 기준 | 합격 값 |
|---|------|---------|
| VE-1 | 추출 포인트 배치 | routes.json의 extraction_points 전수 배치 |
| VE-2 | 스폰→추출 경로 | 각 스폰 존에서 추출 포인트까지 최소 2개 경로 존재 |
| VE-3 | 건물 밀도 | BR 대비 2배 이상 (spacing_m 감소 확인) |
| VE-4 | 노출 구간 | POI 간 연속 노출 200m 이하 |
| VE-5 | 병목 존재 | key_routes의 choke_points에 좁은 통로/벽 존재 |

---

*끝*
