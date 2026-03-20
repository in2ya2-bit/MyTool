# 대형 구조물 외관/실내 생성 도움 툴 — 기획문서 v1.1

> **관련 문서**: `LevelTool_DesignerIntent_보완문서.md` v4.4  
> **버전**: v1.1 (구현 검증 보완 — P0 3건 + P1 4건 + P2 5건)  
> **변경 사유**: 구현 관점 기술 검증 결과 P0(필수) 3건 + P1(중요) 4건 + P2(권장) 5건 보완. 솔리드→모듈러 전환 절차, 비정형 Footprint, 데이터 Truth 원칙, 세션 복원, 산출 알고리즘, Zone 연쇄 효과, 성능 예산, 파괴 상태, 커스텀 템플릿, 복합 건물, 아트 핸드오프, C++ API  
> **목적**: 공장·병원·학교 등 대형 구조물의 외관 형태와 진입 가능한 실내 공간을 화이트박스 수준으로 빠르게 생성하여 게임플레이 프로토타이핑을 지원

---

## 목차

1. [기획의도 및 범위](#기획의도-및-범위)
2. [대상 구조물 유형 분류](#대상-구조물-유형-분류)
3. [레퍼런스 분석](#레퍼런스-분석)
   - 대형 구조물 실내 설계 원칙 12항
4. [외관 생성 시스템](#외관-생성-시스템)
   - Footprint→외벽 모듈 조립 | 파사드 규칙 | 층수 결정
5. [실내 레이아웃 생성 시스템](#실내-레이아웃-생성-시스템)
   - 5.1 Zone 할당 | 5.2 Room BSP 분할 | 5.3 Corridor 연결 | 5.4 Whitebox 조립
6. [기획자 편집 워크플로우](#기획자-편집-워크플로우)
   - 시나리오 A: 폐공장 거점 (~8분) | 시나리오 B: 학교 CQB (~5분)
7. [게임플레이 규칙](#게임플레이-규칙)
   - 엄폐물 | 교전 거리 | 동선 | 수직 전투 | **G-1~G-12 산출 알고리즘**
8. [내부 메커니즘](#내부-메커니즘)
   - **솔리드→모듈러 전환** | **비직사각형 Footprint** | **Edit Layer ↔ structure_layout.json 관계**
   - **Zone 비율 연쇄 효과** | **세션 복원** | **에러 핸들링** | **성능 예산**
9. [기존 시스템 통합](#기존-시스템-통합)
   - buildings.json 확장 | Edit Layer 신규 타입 | Building Pool 확장
10. [데이터 포맷](#데이터-포맷)
    - Structure Template | Structure Layout | Whitebox Module Registry
11. [UI 와이어프레임](#ui-와이어프레임)
12. [Phase 및 테스트 기준](#phase-및-테스트-기준)
13. [추가 사양](#추가-사양)
    - **파괴 상태** | **커스텀 템플릿** | **복합 건물** | **아트 핸드오프** | **C++ API**
14. [기획 확정 사항](#기획-확정-사항)

---

## 기획의도 및 범위

### 배경

현재 LevelTool 1단계는 OSM 데이터에서 건물 footprint + 높이를 추출하여 **단일 메쉬 + 스케일링**으로 건물을 배치한다. 이 방식은 맵 전체의 밀도·스카이라인에 효과적이지만, 기획자가 실제로 전투를 설계해야 하는 **대형 구조물의 진입 가능한 실내 공간**은 생성하지 못한다.

Designer Intent 보완문서(v4.4)의 레퍼런스 분석에서도 다음과 같이 **대형 구조물 내부가 핵심 게임플레이 거점**인 사례가 다수 확인된다:

| 레퍼런스 | 대형 구조물 | 실내 게임플레이 |
|----------|-----------|---------------|
| Erangel School | 교실·강당·수영장 복합 | 좁은 공간 다층 CQB. "졸업 후 Rozhok 연계" |
| Erangel Military Base | ㄷ자 생활관, 격납고, 레이더 철탑 | 옥상 파밍, 고지대 저격, 활주로 개방 교전 |
| Erangel Shelter | 지상 표시 없는 4출입구 대형 벙커 | 미로 구조 산탄총/투척무기 강제 |
| Miramar Pecado | 카지노+경기장+호텔 | 고층 건물 다수 저격+시가전 혼합 |
| Tarkov Customs 기숙사 | 3층 주거동 | 계단실 통제, 방별 루트 등급, 마크방 |
| Tarkov Streets | 도시 전체 건물 내부 구현 | 층간 이동, 잠금 문, 보스 영역, BTR 동적 요소 |
| Tarkov Shoreline Resort | 대형 리조트 호텔 | 열쇠 경제 핵심, 3층 내부 전면 교전 |

> **핵심 문제**: 이런 대형 구조물의 실내는 현재 레벨 디자이너가 BSP/메쉬를 수작업으로 제작해야 한다. 맵 1개에 대형 구조물이 5~15개 필요한 상황에서, 반복적인 실내 레이아웃 작업에 소요되는 시간을 줄이는 도구가 필요하다.

### 목적

**대형 구조물의 외관 형태와 진입 가능한 실내 공간을 화이트박스 수준으로 빠르게 생성하고, 기획자가 게임플레이 의도에 맞게 편집할 수 있는 도움 툴을 제공한다.**

### 하는 것 / 하지 않는 것 / 전제 조건

| 하는 것 | 하지 않는 것 | 전제 조건 |
|---------|------------|----------|
| 구조물 유형별 외벽 모듈 자동 조립 | 최종 비주얼 메쉬 제작 (아트팀 영역) | 1단계 건물 배치 완료 |
| 기능 영역(Zone) 기반 실내 배치 자동 생성 | 가구·소품 상세 배치 (별도 도구) | `buildings.json`에 `area_m2` + `type` 존재 |
| BSP/화이트박스 수준 벽·바닥·천장·문·계단 배치 | 파괴 시뮬레이션 메쉬 (런타임 영역) | 모듈러 화이트박스 메쉬 세트 (Phase S-0) |
| 기획자의 방/복도 추가·삭제·이동 편집 | AI 적 배치·스폰 로직 (게임 시스템 영역) | |
| 게임플레이 규칙 자동 검증 (엄폐·동선·사선) | 네트워크·최적화 구현 | |

### 기존 파이프라인에서의 위치

```
[1단계: 실사 생성]
     ↓
[1.5단계: 대형 구조물 내부 생성]  ← 신규
     ↓
[2단계: 의도 기반 수정 (슬라이더/프리셋)]
     ↓  ⇅
[3단계: 공간적 전제조건 진단+제안]
```

1.5단계는 **선택적 단계**이다. 기획자가 대형 구조물을 선택하고 "실내 생성"을 요청할 때만 실행된다. 1.5단계를 거치지 않은 건물은 기존대로 솔리드 메쉬로 유지된다.

| 전환 | 동작 |
|------|------|
| **1단계 → 1.5단계** | 기획자가 뷰포트에서 대형 건물 선택 → "구조물 내부 생성" 실행 |
| **1.5단계 → 2단계** | 생성된 구조물은 `structure_layout.json`에 저장. 2단계 슬라이더·프리셋은 구조물을 단일 POI로 취급 |
| **1.5단계 → 3단계** | 3단계 체크리스트에 "실내 동선 품질" 항목 추가 가능 (Phase S-D) |
| **2단계 → 1.5단계** | 2단계에서 `building_add`로 추가한 건물도 대형이면 1.5단계 적용 가능 |

---

## 대상 구조물 유형 분류

### 대형 구조물 판정 기준

| 조건 | 값 | 근거 |
|------|-----|------|
| 바닥 면적 | ≥ 500m² | 일반 주거(~100m²)의 5배. OSM 데이터 기준 학교·공장 최소 면적 |
| 또는 높이 | ≥ 15m (4층+) | 3층 이하는 일반 건물 프리셋으로 충분 |
| 또는 TypeKey | `Industrial`, `Warehouse`, `Hospital`, `School`, `Church` | 유형 자체가 대형 구조물을 암시 |

> 판정은 `buildings.json` 로드 시 자동. `"is_large_structure": true` 플래그를 부여하고, UI에서 대형 구조물 아이콘(🏭)으로 구분 표시.

### 6종 구조물 유형 정의

#### Type 1: 공장 / 산업시설 (Factory / Industrial)

| 항목 | 값 |
|------|-----|
| 기존 TypeKey | `BP_Building_Industrial` |
| 층수 범위 | 1~3층 |
| 층고 | 5.0~8.0m (높은 천장, 크레인 레일) |
| 대표 면적 | 800~5000m² |
| 특징 | 대형 개방 공간 + 고가 캣워크 + 좁은 제어실 |

| 기능 영역 (Zone) | 비율 | 방 크기 범위 | 게임플레이 |
|-----------------|------|------------|----------|
| 생산라인 (Production) | 40% | 20×30m ~ 40×60m | 대형 개방 공간. 장거리 사선, 기계 엄폐물 |
| 제어실 (Control Room) | 10% | 4×6m ~ 6×8m | 소형 밀폐 방. CQB, 유리창 사선 |
| 창고 (Storage) | 25% | 10×15m ~ 20×30m | 선반 사이 사선 차단. 중거리 교전 |
| 로딩독 (Loading Dock) | 10% | 8×12m ~ 12×20m | 외부 노출 + 셔터. 진입/탈출 지점 |
| 유틸리티 (Utility) | 10% | 3×4m ~ 5×6m | 보일러실, 전기실. 은닉 포인트 |
| 복도/통로 (Corridor) | 5% | 폭 2.5~4m | 캣워크 포함 |

#### Type 2: 창고 (Warehouse)

| 항목 | 값 |
|------|-----|
| 기존 TypeKey | `BP_Building_Warehouse` |
| 층수 범위 | 1~2층 |
| 층고 | 6.0~10.0m |
| 대표 면적 | 500~3000m² |
| 특징 | 단일 대형 공간 + 사무실 칸막이 |

| 기능 영역 (Zone) | 비율 | 방 크기 범위 | 게임플레이 |
|-----------------|------|------------|----------|
| 적재구역 (Storage Floor) | 60% | 단일 공간 전체 | 선반·컨테이너 그리드. 은엄폐 반복 |
| 사무실 (Office) | 15% | 4×5m ~ 8×10m | 2층 메자닌 위 감시 위치 |
| 로딩독 (Loading Dock) | 15% | 6×10m ~ 10×15m | 셔터 도어, 외부 노출 |
| 유틸리티 (Utility) | 10% | 3×4m | 좁은 공간, 은닉 |

#### Type 3: 병원 (Hospital)

| 항목 | 값 |
|------|-----|
| 기존 TypeKey | `BP_Building_Hospital` |
| 층수 범위 | 3~8층 |
| 층고 | 3.2~4.0m |
| 대표 면적 | 1000~8000m² (per floor) |
| 특징 | 긴 복도 + 균일한 방 반복 + 계단실/엘리베이터 병목 |

| 기능 영역 (Zone) | 비율 | 방 크기 범위 | 게임플레이 |
|-----------------|------|------------|----------|
| 로비/접수 (Lobby) | 10% (1F만) | 15×20m ~ 20×30m | 개방 아트리움. 다층 사선 |
| 병실 (Ward) | 35% | 4×6m ~ 5×7m | 반복 구조 복도+방. 문 열기 긴장감 |
| 수술실/처치실 (OR) | 10% | 6×8m ~ 8×10m | 장비 엄폐, 유리 관찰창 |
| 복도 (Corridor) | 25% | 폭 2.5~3.5m | 긴 직선 사선. T자/L자 코너 교전 |
| 계단실/EV (Vertical) | 10% | 4×4m ~ 5×5m | 층간 이동 병목. 그레네이드 취약 |
| 유틸리티/지하 (Utility) | 10% | 다양 | 보일러, 세탁, 영안실. 탐험 리워드 |

#### Type 4: 학교 (School)

| 항목 | 값 |
|------|-----|
| 기존 TypeKey | `BP_Building_School` |
| 층수 범위 | 2~4층 |
| 층고 | 3.2~3.5m (체육관 6~8m) |
| 대표 면적 | 800~4000m² (per floor) |
| 특징 | 반복 교실+복도 패턴 + 체육관/강당 대형 공간 |

| 기능 영역 (Zone) | 비율 | 방 크기 범위 | 게임플레이 |
|-----------------|------|------------|----------|
| 교실 (Classroom) | 35% | 7×9m ~ 9×11m | 책상 엄폐, 문 2개(전후), 창문 사선 |
| 체육관/강당 (Gymnasium) | 15% | 18×30m ~ 25×40m | 대형 개방 공간. 무대/관중석 고저차 |
| 복도 (Corridor) | 25% | 폭 2.5~3.5m | 긴 직선 + 로커 엄폐 |
| 교무실/행정 (Admin) | 10% | 5×6m ~ 8×10m | 소형 방 클러스터 |
| 식당/매점 (Cafeteria) | 10% | 10×15m ~ 15×20m | 중형 개방, 테이블 엄폐 |
| 계단실 (Stairs) | 5% | 3×4m ~ 4×5m | 층간 병목 |

#### Type 5: 상업시설 (Commercial / Mall)

| 항목 | 값 |
|------|-----|
| 기존 TypeKey | `BP_Building_Commercial`, `BP_Building_Retail` |
| 층수 범위 | 1~5층 |
| 층고 | 3.5~4.5m |
| 대표 면적 | 600~6000m² (per floor) |
| 특징 | 개방 아트리움 + 에스컬레이터 + 다양한 매장 크기 |

| 기능 영역 (Zone) | 비율 | 방 크기 범위 | 게임플레이 |
|-----------------|------|------------|----------|
| 아트리움/통로 (Atrium) | 20% | 중앙 개방 | 다층 관통 사선. 난간 엄폐 |
| 매장 (Shop) | 40% | 5×8m ~ 15×20m | 다양한 크기. 셔터/유리벽 |
| 식당가 (Food Court) | 15% | 15×20m ~ 20×25m | 개방 중형 공간. 테이블 엄폐 |
| 주차장 (Parking) | 15% (B1~) | 전층 개방 | 기둥 그리드 엄폐. 차량 활용 |
| 계단/에스컬레이터 | 10% | 다양 | 수직 이동 다양성 |

#### Type 6: 교회 / 성당 (Church / Cathedral)

| 항목 | 값 |
|------|-----|
| 기존 TypeKey | `BP_Building_Church` |
| 층수 범위 | 1~2층 + 첨탑 |
| 층고 | 6.0~15.0m (본당), 첨탑 20~40m |
| 대표 면적 | 300~2000m² |
| 특징 | 높은 천장 본당 + 좁은 첨탑 저격 포인트 |

| 기능 영역 (Zone) | 비율 | 방 크기 범위 | 게임플레이 |
|-----------------|------|------------|----------|
| 본당 (Nave) | 50% | 10×20m ~ 15×30m | 높은 천장 개방. 기둥 엄폐 |
| 제단/성가대석 (Chancel) | 15% | 8×10m | 고지대 위치 |
| 첨탑 (Tower) | 10% | 3×3m ~ 4×4m | 나선 계단+최상단 저격 포인트 |
| 지하실 (Crypt) | 15% | 6×8m ~ 10×15m | 저조도 CQB. 탐험 리워드 |
| 부속실 (Vestry) | 10% | 3×4m ~ 5×6m | 소형 방 |

---

## 레퍼런스 분석

### 대형 구조물 실내가 등장하는 레퍼런스 상세

#### R-1. Tarkov — Customs 기숙사 (3층 주거동)

| 항목 | 분석 |
|------|------|
| 구조 | 3층 건물 2동 (2층동 + 3층동). 외부 계단 + 내부 계단 이중 동선 |
| 실내 패턴 | 중복도 양쪽 방 배치. 방마다 잠금 문 (열쇠 경제) |
| 게임플레이 | 계단실 진입 병목 → 복도 장악 → 방 클리어링 3단계 교전 흐름 |
| 수직 전투 | 계단실 그레네이드, 층간 발소리 정보전 |
| 설계 원칙 | **중복도+균일 방 = 반복 구조의 교전 리듬** |

#### R-2. Tarkov — Streets 건물군 (도시 전체)

| 항목 | 분석 |
|------|------|
| 구조 | 다양한 규모의 상업·주거·공공 건물. 건물 간 지붕/발코니 연결 |
| 실내 패턴 | 건물마다 고유 레이아웃. 파괴된 벽/바닥으로 즉석 루트 |
| 게임플레이 | 건물 내부 ↔ 도로 사이 시야 교환. 창문 포지셔닝 |
| 수직 전투 | 해치, 무너진 바닥, 발코니 드롭다운 |
| 설계 원칙 | **건물마다 고유 레이아웃 = 맵 학습 보상. 파괴 개구부 = 즉석 동선** |

#### R-3. Tarkov — Shoreline Resort (대형 호텔)

| 항목 | 분석 |
|------|------|
| 구조 | 동쪽/서쪽 날개 + 중앙 로비. 3층. 외부에서 관찰 가능한 창문 |
| 실내 패턴 | 호텔 객실 반복 (편복도). 객실 크기 2종(일반/스위트) |
| 게임플레이 | 열쇠 방 = 고가 루트. 복도 장악이 층 통제의 핵심 |
| 수직 전투 | 중앙 계단실 3개 + 외부 비상 계단 |
| 설계 원칙 | **편복도+열쇠 방 = 위험-보상 경제. 날개 구조 = 진출 판단** |

#### R-4. Erangel — School + Apartments

| 항목 | 분석 |
|------|------|
| 구조 | 학교 본관(교실·강당·수영장) + 인접 아파트 단지 |
| 실내 패턴 | 교실 반복 + 대형 특수 공간(체육관/수영장) 혼합 |
| 게임플레이 | 좁은 공간 다층 CQB. 옥상 파밍 + 주변 진출 루트 |
| 수직 전투 | 계단실 + 옥상 접근. 파쿠르로 지붕 이동 |
| 설계 원칙 | **반복 교실 + 특수 대형 공간 = 예측 가능 + 불확실성 혼합** |

#### R-5. Erangel — Military Base

| 항목 | 분석 |
|------|------|
| 구조 | ㄷ자 생활관, 격납고(대형 개방), 레이더 철탑(수직), 활주로(외부 개방) |
| 실내 패턴 | 유형별 완전히 다른 내부 구조. 하나의 거점에 다양한 교전 스타일 |
| 게임플레이 | 실내(생활관 CQB) + 반외부(격납고) + 외부(활주로) 혼합 |
| 수직 전투 | 레이더 철탑 극한 저격. 생활관 옥상 |
| 설계 원칙 | **하나의 POI 내 다양한 구조물 유형 = 교전 거리 다양성** |

#### R-6. Erangel — Shelter (지하 벙커)

| 항목 | 분석 |
|------|------|
| 구조 | 지상 표시 없는 4출입구 대형 지하 벙커 |
| 실내 패턴 | 미로 복도 + 방 혼합. 낮은 천장, 좁은 통로 |
| 게임플레이 | 산탄총/투척무기 강제. 탐험 리워드 설계의 원형 |
| 수직 전투 | 진입 계단에서 내부까지 수직 하강 |
| 설계 원칙 | **숨겨진 구조물 = 탐험 동기. 미로 = 무기 선택 강제** |

#### R-7. Rainbow Six Siege — 맵 설계 체계

| 항목 | 분석 |
|------|------|
| 구조 | 모듈러 방+복도 시스템. 파괴 가능 벽/바닥/천장 |
| 실내 패턴 | 방마다 고유 이름과 기능. 방 크기 2~3종 혼합 |
| 게임플레이 | 방벽·해치 파괴로 실시간 동선 생성. 수직 플레이 핵심 |
| 수직 전투 | 해치 파괴, 천장 돌파, 래펠링 |
| 설계 원칙 | **모듈러 방 + 파괴 가능 벽 = 동적 레이아웃. 명명된 방 = 커뮤니케이션** |

### 대형 구조물 실내 설계 원칙 — 12항

레퍼런스 R-1~R-7에서 도출한 공통 원칙.

| # | 원칙 | 근거 | 적용 방법 |
|---|------|------|----------|
| S-1 | **최소 2개 진입/탈출 경로** | R-1(기숙사 외부+내부 계단), R-6(벙커 4출입구) | 외벽에 출입구 ≥ 2개 자동 배치. 층당 계단실 ≥ 2개 |
| S-2 | **반복 구조 + 특수 공간 혼합** | R-4(교실+체육관), R-3(객실+로비) | Zone 비율에 반복형(교실/병실) + 대형(체육관/로비)을 반드시 포함 |
| S-3 | **복도에서 방으로의 전환 리듬** | R-1(중복도+방), R-4(편복도+교실) | 복도 10~15m마다 문 또는 분기점 배치 |
| S-4 | **수직 전투 지점 확보** | R-2(해치/무너진 바닥), R-7(해치/래펠) | 2층+ 구조물은 층당 사선 개방 지점(해치/발코니/보이드) ≥ 1개 |
| S-5 | **교전 거리 다양성** | R-5(CQB 생활관+장거리 활주로) | 방 크기 분포: CQB(<5m) 30% + 중거리(5~15m) 50% + 장거리(>15m) 20% |
| S-6 | **층간 이동 병목 통제** | R-1(계단실 진입 병목), R-3(중앙 계단 3개) | 계단실은 복도 사선에서 직접 보이지 않도록 배치 (코너/전실) |
| S-7 | **창문 = 외부↔내부 시야 교환** | R-2(건물↔도로 시야), R-3(외부 관찰 가능) | 외벽 방에 창문 자동 배치. 창문 위치에서 외부 사선 검증 |
| S-8 | **은닉/탐험 공간** | R-6(숨겨진 벙커), R-1(잠금 방) | 유틸리티 Zone을 접근 비직관적 위치에 배치 |
| S-9 | **예측 가능한 레이아웃 문법** | R-1(균일 방 반복), R-7(명명된 방) | 동일 Zone 내 방은 유사 크기·형태로 반복. 특수 공간은 명확히 구분 |
| S-10 | **고저차 활용** | R-5(레이더 철탑), R-6(지하 하강) | 메자닌, 반층 차이, 캣워크 등 수평면 외 고저차 요소 포함 |
| S-11 | **건물 간 연결 가능성** | R-2(지붕/발코니 연결) | 인접 구조물 간 2층+ 연결 통로/발코니 옵션 제공 |
| S-12 | **내부 랜드마크** | R-4(수영장/강당), R-5(격납고) | 구조물당 1개 이상 시각적으로 구분되는 대형 특수 공간 배치 |

---

## 외관 생성 시스템

### 개요

기존 솔리드 메쉬 건물을 **모듈러 외벽 조합**으로 대체하여, 실내 공간 생성의 전제조건(벽 개구부, 문, 창문)을 확보한다.

### Footprint → 외벽 모듈 조립 파이프라인

```
Footprint 폴리곤 (OSM/Edit Layer)
     ↓
Edge 분해 (각 변을 직선 세그먼트로)
     ↓
층수 결정 (높이 / 층고)
     ↓
층별 × Edge별 외벽 모듈 선택
     ↓
코너·기둥·지붕 모듈 배치
     ↓
출입구·창문 배치 (파사드 규칙)
```

### Edge 분해 규칙

| 단계 | 동작 | 상세 |
|------|------|------|
| 1 | Footprint 폴리곤을 Edge 목록으로 분해 | `[P0→P1, P1→P2, ..., Pn→P0]` |
| 2 | 각 Edge를 모듈 폭(기본 4m)으로 분할 | Edge 길이 25m → 6개 모듈 + 1m 잔여 |
| 3 | 잔여 길이 처리 | ≤ 1m: 인접 모듈 확장. > 1m: 좁은 모듈 삽입 |
| 4 | 코너 포인트에 코너 모듈 배치 | 각도별 처리: 90°=직각 코너, 기타=범용 코너 |

### 파사드 규칙 — 구조물 유형별 외벽 모듈 선택

| 구조물 유형 | 1층 모듈 비율 | 상층 모듈 비율 | 특수 요소 |
|-----------|-------------|-------------|---------|
| 공장 | 셔터 30%, 벽 50%, 창문 20% | 고창(clerestory) 60%, 벽 40% | 로딩독 셔터(폭 6~8m) |
| 창고 | 셔터 40%, 벽 50%, 문 10% | (보통 1층) | 대형 롤링 셔터 |
| 병원 | 유리문 20%, 창문 60%, 벽 20% | 창문 70%, 벽 30% | 정면 로비 유리 파사드 |
| 학교 | 문 15%, 창문 55%, 벽 30% | 창문 60%, 벽 40% | 체육관 측면 높은 벽 |
| 상업 | 유리 50%, 문 20%, 벽 30% | 유리 40%, 벽 60% | 쇼윈도, 간판 영역 |
| 교회 | 문 10%, 스테인드글라스 30%, 벽 60% | 장식벽 50%, 창문 20%, 벽 30% | 첨탑, 로즈윈도우 |

### 층수 자동 결정

| 데이터 소스 | 산출 방법 | 우선순위 |
|-----------|----------|---------|
| OSM `building:levels` 태그 | 직접 사용 | 1 (최우선) |
| OSM `height` 태그 | `height / FLOOR_HEIGHT_BY_TYPE[type]` 반올림 | 2 |
| `buildings.json` `height_m` | 동일 공식 | 3 |
| 기본값 | 유형별 기본 층수 (공장 1, 병원 5, 학교 3...) | 4 (폴백) |

### 지붕 모듈

| 지붕 유형 | 적용 구조물 | 모듈 |
|----------|-----------|------|
| 평지붕 (Flat) | 병원, 학교, 상업 | `SM_Roof_Flat` + 옥상 난간 |
| 박공 (Gable) | 창고, 주거형 | `SM_Roof_Gable` |
| 톱날 (Sawtooth) | 공장 | `SM_Roof_Sawtooth` (채광창) |
| 첨탑 (Spire) | 교회 | `SM_Roof_Spire` |

---

## 실내 레이아웃 생성 시스템

### 생성 파이프라인 개요

```
입력: Footprint + 유형 + 층수
     ↓
[5.1] Zone 할당 — 기능 영역을 대블록으로 배치
     ↓
[5.2] Room BSP 분할 — 각 Zone 내부를 방으로 분할
     ↓
[5.3] Corridor 연결 — 방 사이 복도 + 계단실 배치
     ↓
[5.4] Whitebox 조립 — 벽·바닥·천장·문·계단 모듈 배치
     ↓
출력: structure_layout.json + UE5 Actor 배치
```

### 5.1 Zone 할당

Footprint 내부를 기능 영역(Zone)으로 분할한다. 각 Zone은 직사각형 또는 L자형 블록이다.

**알고리즘: Treemap Squarified Layout**

| 단계 | 동작 |
|------|------|
| 1 | 구조물 유형에서 Zone 비율 테이블 로드 (위 유형 정의 참조) |
| 2 | Footprint의 바운딩 박스를 기준 영역으로 설정 |
| 3 | Zone을 면적 비율 내림차순 정렬 |
| 4 | Squarified Treemap 알고리즘으로 직사각형 분할 |
| 5 | 각 Zone 블록에 `zone_id`, `zone_type`, AABB 할당 |
| 6 | Footprint 외부에 걸치는 셀 제거 (비직사각형 footprint 대응) |

**층별 Zone 배치 규칙**:

| 규칙 | 상세 |
|------|------|
| 로비/접수 | 1층에만 배치. 정문 Edge에 인접 |
| 주차장 | B1층 이하에 배치 |
| 유틸리티 | 1층 또는 지하에 배치. 외벽 인접 아닌 내부 위치 우선 |
| 반복 영역 (교실/병실) | 2층 이상에 배치. 균일 분배 |
| 대형 특수 공간 (체육관/강당) | 1~2층 통층 (천장 높이 필요). 외벽 한 면 이상 접함 |
| 첨탑 | 최상층 위 별도 구조 |

**Zone 비율 슬라이더**:

기획자가 Zone 비율을 조정할 수 있다. 조정 시 Treemap 재계산.

```
[교실/병실  ] [■■■■■■■□□□] 35%     ← 드래그 조정
[체육관/강당] [■■□□□□□□□□] 15%
[복도      ] [■■■■■□□□□□] 25%
[행정      ] [■■□□□□□□□□] 10%
[식당      ] [■■□□□□□□□□] 10%
[계단실    ] [■□□□□□□□□□]  5%
                              합계: 100%
```

### 5.2 Room BSP 분할

각 Zone 내부를 개별 방으로 분할한다.

**알고리즘: Binary Space Partitioning (BSP)**

| 단계 | 동작 |
|------|------|
| 1 | Zone AABB를 루트 노드로 설정 |
| 2 | Zone 유형의 방 크기 범위 (min/max) 로드 |
| 3 | 루트 노드 크기 > max 방 크기 × 2 이면 분할 |
| 4 | 분할 방향: 종횡비 > 1.5 이면 긴 축 분할, 아니면 랜덤 |
| 5 | 분할 위치: 40~60% 랜덤 (균등 분할 방지 → 자연스러운 크기 변화) |
| 6 | 재귀 분할, 리프 노드 크기가 min~max 범위 내일 때 중단 |
| 7 | 리프 노드 = 방. `room_id`, 크기, 위치 할당 |

**특수 공간 처리 (분할 제외)**:

| Zone 유형 | 처리 |
|----------|------|
| 생산라인 (공장) | BSP 분할하지 않음. 단일 대공간 유지 |
| 체육관/강당 (학교) | BSP 분할하지 않음. 통층 처리 |
| 아트리움 (상업) | BSP 분할하지 않음. 다층 보이드 |
| 로비 (병원) | BSP 분할하지 않음. 2층 이상 보이드 옵션 |
| 적재구역 (창고) | BSP 분할하지 않음. 단일 대공간 |

**방 크기 제약 테이블**:

| Zone 유형 | 최소 (m) | 최대 (m) | 종횡비 상한 |
|----------|---------|---------|-----------|
| 교실 | 7×9 | 9×11 | 1:1.6 |
| 병실 | 4×5 | 5×7 | 1:1.8 |
| 사무실 | 3×4 | 8×10 | 1:2.5 |
| 제어실 | 4×5 | 6×8 | 1:1.6 |
| 매장 | 5×5 | 15×20 | 1:3.0 |
| 부속실/유틸리티 | 2×3 | 5×6 | 1:2.0 |

### 5.3 Corridor 연결

방 사이를 복도로 연결하고 수직 이동 수단(계단/엘리베이터)을 배치한다.

**복도 패턴 선택**:

| 패턴 | 설명 | 적용 유형 | 조건 |
|------|------|----------|------|
| 편복도 (Single-loaded) | 복도 한쪽에만 방 배치 | 호텔, 아파트 | Zone 폭 < 12m |
| 중복도 (Double-loaded) | 복도 양쪽에 방 배치 | 학교, 병원, 기숙사 | Zone 폭 ≥ 12m |
| 루프 (Ring) | 중앙 코어 주위 순환 복도 | 사무실, 상업 | Zone 정방형 (종횡비 < 1.3) |
| 자유형 (Open) | 복도 없음, 대공간 내 동선 | 공장, 창고, 체육관 | 분할 제외 Zone |

**복도 생성 알고리즘**:

| 단계 | 동작 |
|------|------|
| 1 | Zone 내 BSP 리프(방) 목록 확보 |
| 2 | 복도 패턴 자동 선택 (위 테이블 기준) |
| 3 | 복도 중심선 생성 (Zone 긴 축 방향) |
| 4 | 복도 폭: 2.5m (기본), 병원 3.0m, 상업 3.5m |
| 5 | 방 AABB를 복도 측으로 축소 (복도 공간 확보) |
| 6 | 각 방-복도 접점에 문(Door) 위치 할당 |
| 7 | T자·L자 교차점에 분기 노드 할당 |

**계단실 배치 규칙**:

| 규칙 | 값 | 근거 |
|------|-----|------|
| 층당 최소 계단실 수 | 2개 | S-1 원칙 (최소 2개 탈출 경로) |
| 계단실 최대 간격 | 40m | 소방 피난 거리 기준 차용 |
| 계단실 위치 | 복도 양 끝단 우선 | 최대 이격 → 동선 다양성 |
| 계단실 크기 | 3×4m ~ 5×5m | 180° 꺾임 계단 최소 공간 |
| 층간 연속성 | 계단실은 전 층에서 동일 XY 위치 | 구조적 일관성 |

**출입구 배치 규칙**:

| 규칙 | 값 | 근거 |
|------|-----|------|
| 최소 출입구 수 | 2개 | S-1 원칙 |
| 출입구 간 최소 이격 | 건물 둘레의 25% | 포위 방지 — 다른 면에 분산 |
| 출입구 위치 우선순위 | 1. 도로 인접 Edge → 2. 긴 Edge → 3. 랜덤 | 접근성 |
| 출입구 크기 | 문(1.2m), 이중문(2.4m), 셔터(4~8m) | 유형별 |

### 5.4 Whitebox 조립

생성된 레이아웃 정보를 UE5 Actor로 물리적 배치한다.

**모듈러 화이트박스 피스 목록**:

| 카테고리 | 모듈 ID | 기본 크기 (m) | 비고 |
|---------|---------|-------------|------|
| 벽 | `WB_Wall` | 4×0.2×3.2 (W×D×H) | 기본 벽 |
| 벽+창문 | `WB_Wall_Window` | 4×0.2×3.2 | 중앙 개구부 1.2×1.5m |
| 벽+문 | `WB_Wall_Door` | 4×0.2×3.2 | 측면 개구부 1.0×2.2m |
| 벽+셔터 | `WB_Wall_Shutter` | 6×0.2×4.0 | 산업용 대형 개구부 |
| 코너 | `WB_Corner` | 0.2×0.2×3.2 | 외벽 모서리 |
| 기둥 | `WB_Pillar` | 0.4×0.4×3.2 | 내부 구조 기둥 |
| 바닥 | `WB_Floor` | 4×4×0.2 | 타일링 가능 |
| 천장 | `WB_Ceiling` | 4×4×0.2 | 바닥과 동일, 상부 배치 |
| 계단 | `WB_Stairs` | 3×4×3.2 | 1층분 높이, 180° 꺾임 |
| 계단 (직선) | `WB_Stairs_Straight` | 1.5×6×3.2 | 직선 계단 |
| 난간 | `WB_Railing` | 4×0.1×1.1 | 발코니, 캣워크용 |
| 캣워크 | `WB_Catwalk` | 4×1.5×0.1 | 공장 고가 통로 |
| 지붕 (평) | `WB_Roof_Flat` | 4×4×0.2 | 바닥과 동일 |
| 지붕 (경사) | `WB_Roof_Slope` | 4×4×변동 | 박공/톱날 구성 |

**조립 알고리즘**:

| 단계 | 동작 |
|------|------|
| 1 | 층별 바닥 모듈 배치 (방+복도 영역을 4×4m 그리드로 타일링) |
| 2 | 외벽 모듈 배치 (Edge 분해 결과 + 파사드 규칙) |
| 3 | 내벽 모듈 배치 (방 경계선을 따라 `WB_Wall` 또는 `WB_Wall_Door`) |
| 4 | 방-복도 접점에 문 모듈 배치 |
| 5 | 계단실 위치에 계단 모듈 배치 |
| 6 | 천장 모듈 배치 (바닥과 동일 그리드, 층고 높이) |
| 7 | 지붕 모듈 배치 (최상층 위) |
| 8 | 캣워크/난간 배치 (공장 대공간, 메자닌) |

**성능 고려**:

| 항목 | 목표 | 방법 |
|------|------|------|
| 모듈 인스턴싱 | HISM 활용 | 동일 모듈은 `UHierarchicalInstancedStaticMeshComponent`로 묶음 |
| 모듈 수 제한 | 건물당 < 2000개 | 4m 그리드 기준 1000m² × 3층 ≈ 1500 모듈 |
| 생성 시간 | < 3초 (1000m² 3층 기준) | BSP+조립 병렬화 불필요 (규모 작음) |
| 콜리전 | BlockAll | 화이트박스 단계에서는 단순 박스 콜리전 |

---

## 기획자 편집 워크플로우

### 편집 도구

기본 골격은 자동 생성되지만, 기획자가 게임플레이 의도에 맞게 편집할 수 있다.

| 도구 | 조작 | 결과 |
|------|------|------|
| **방 선택** | 뷰포트/평면도에서 방 클릭 | 방 속성 패널 표시 (크기, Zone 유형) |
| **방 크기 조정** | 방 Edge 드래그 | 인접 방/복도 자동 축소·확대. 최소 크기 제약 적용 |
| **방 삭제** | 방 선택 → Delete | 해당 방을 복도로 전환 (공간 흡수) |
| **방 추가** | 복도/빈 공간 클릭 → "방 추가" | 복도를 분할하여 새 방 생성 |
| **방 분할** | 방 선택 → "분할" → 분할선 그리기 | BSP 수동 분할 |
| **방 병합** | 인접 방 2개 선택 → "병합" | 벽 제거, 단일 방으로 합침 |
| **문 추가/제거** | 벽 클릭 → "문 추가/제거" | `WB_Wall` ↔ `WB_Wall_Door` 교체 |
| **창문 추가/제거** | 외벽 클릭 → "창문 추가/제거" | `WB_Wall` ↔ `WB_Wall_Window` 교체 |
| **계단 이동** | 계단실 드래그 | XY 위치 이동 (전층 동기화) |
| **계단 추가** | 빈 공간/방 → "계단 추가" | 해당 위치에 계단실 생성. 상하층 관통 |
| **캣워크 추가** | 대공간 내 2점 지정 | 고가 통로 생성 (공장/창고) |
| **출입구 추가/이동** | 외벽 클릭 → "출입구" | 해당 위치에 출입 문/셔터 배치 |
| **층 추가/삭제** | Structure 패널 → "층 추가/삭제" | 최상층 위 추가 또는 최상층 삭제 |
| **레이아웃 재생성** | "재생성" 버튼 | 현재 파라미터로 전체 재생성 (편집 내용 초기화 경고) |

### 편집의 비파괴성

모든 편집은 `structure_layout.json`에 기록되며, 기존 Edit Layer 체계와 유사한 비파괴 방식을 따른다.

| 편집 | 저장 형태 | Undo |
|------|----------|------|
| 방 크기 조정 | `room_resize` 오퍼레이션 | 1 Transaction |
| 방 삭제 | `room_remove` 오퍼레이션 | 1 Transaction |
| 문/창문 변경 | `wall_modify` 오퍼레이션 | 1 Transaction |
| 계단 추가 | `stairs_add` 오퍼레이션 | 1 Transaction (전층) |
| 레이아웃 재생성 | 전체 교체 | 1 Transaction |

### 기획자 워크플로우 시나리오

#### 시나리오 A: "폐공장 거점을 만드는 기획자" (~8분)

| 시간 | 행동 | 시스템 반응 |
|------|------|-----------|
| 0:00 | 1단계에서 생성된 맵에서 Industrial 건물(1500m²) 선택 | 건물 하이라이트, 속성 표시: `BP_Building_Industrial`, 1500m², 높이 12m |
| 0:30 | 우클릭 → "구조물 내부 생성" | Structure Editor 패널 열림. 유형: 공장(자동 인식) |
| 1:00 | Zone 비율 확인: 생산 40%, 창고 25%, 제어실 10%, 로딩독 10%, 유틸 10%, 복도 5% | 자동 생성된 기본 비율 표시 |
| 1:30 | "생성" 버튼 클릭 | Zone 할당 → BSP → Corridor → Whitebox 조립. 2초 소요. 뷰포트에 화이트박스 표시 |
| 2:00 | 층별 평면도에서 결과 확인. 생산라인 대공간 OK | — |
| 2:30 | "제어실이 생산라인을 내려다봐야 한다" → 제어실 방을 2층 메자닌으로 드래그 | 제어실이 2층 높이로 이동. 캣워크 자동 연결 |
| 3:30 | "로딩독이 도로 쪽을 향해야 한다" → 로딩독 Zone을 도로 인접 Edge로 이동 | 외벽 셔터 자동 재배치 |
| 4:30 | "비밀 방을 추가하고 싶다" → 유틸리티 영역 뒤쪽 벽 클릭 → "방 추가" | 2×3m 소형 방 생성. 문 1개 |
| 5:30 | "캣워크를 추가해서 수직 전투 포인트 확보" → 대공간 내 2점 지정 | 캣워크 + 난간 생성 (높이 5m) |
| 6:30 | 게임플레이 검증 실행 → S-5 Warning (장거리 사선 > 30m 구간) | 제안: "중간 기둥 추가 또는 선반 배치 권장" |
| 7:30 | "기둥 2개 추가" → 대공간 내 2점 클릭 | WB_Pillar 배치 |
| 8:00 | 만족 → 저장 | `structure_layout.json` 저장 |

#### 시나리오 B: "학교 건물 CQB 영역을 빠르게 구성하는 기획자" (~5분)

| 시간 | 행동 | 시스템 반응 |
|------|------|-----------|
| 0:00 | School 건물(2000m²) 선택 → "구조물 내부 생성" | 유형: 학교(자동). 3층 |
| 0:30 | Zone 비율 기본값 확인. "체육관을 더 크게" → 체육관 15%→25% 조정 | Zone 비율 재계산. 교실 35%→25%로 자동 감소 |
| 1:00 | "생성" 클릭 | 3층 × Zone 할당 → BSP → Whitebox. 2.5초 |
| 1:30 | 평면도 확인. 1층: 로비+체육관+식당. 2~3층: 교실+복도 | — |
| 2:00 | "2층 복도가 너무 직선적이다" → 복도 중간에 "방 추가" → T자 분기 생성 | 복도가 T자로 분기. 새 방 자동 생성 |
| 2:30 | "체육관에 관중석 고저차 추가" → 체육관 내 영역 선택 → "고저차: +1.5m" | 관중석 영역 바닥 높이 상승. 계단 자동 연결 |
| 3:30 | "옥상 접근 추가" → 3층 계단실 위 → "계단 추가 (옥상)" | 옥상 출입구 + 난간 생성 |
| 4:00 | 게임플레이 검증 → Pass (12항 중 11항 통과, S-10 Warning: "고저차 활용 부족") | — |
| 4:30 | "이 정도면 OK" → 저장 | `structure_layout.json` 저장 |
| 5:00 | 2단계로 전환하여 주변 밀도·동선 슬라이더 조정 | 학교 구조물은 단일 POI로 유지 |

---

## 게임플레이 규칙

### 자동 검증 체크리스트

생성된 실내 레이아웃에 대해 게임플레이 품질을 자동 검증한다. 설계 원칙 12항(S-1~S-12)에 대응.

| # | 검증 항목 | Pass 조건 | Warning 조건 | Fail 조건 | 자동 제안 |
|---|----------|----------|-------------|----------|---------|
| G-1 | 진입/탈출 경로 (S-1) | 외벽 출입구 ≥ 2 AND 서로 다른 면 | 출입구 ≥ 2이나 동일 면 | 출입구 < 2 | 부족 면에 출입구 추가 |
| G-2 | 반복+특수 혼합 (S-2) | Zone 유형 ≥ 3종 AND 대형 특수 공간 ≥ 1 | Zone 유형 ≥ 2종 | Zone 유형 1종 | 특수 Zone 추가 제안 |
| G-3 | 복도 리듬 (S-3) | 복도 10~15m마다 문/분기 | 분기 간격 15~20m | 직선 > 20m 구간 존재 | 분기점 또는 앨코브 추가 |
| G-4 | 수직 전투 지점 (S-4) | 2층+ 시 층당 개방 지점 ≥ 1 | 개방 지점이 계단실뿐 | 개방 지점 0 | 발코니/보이드 추가 |
| G-5 | 교전 거리 다양성 (S-5) | CQB(<5m) 20~40%, 중(5~15m) 40~60%, 장(>15m) 10~30% | 1개 범위가 기준 초과/미달 | 1개 범위 0% | 방 크기 조정 제안 |
| G-6 | 계단실 병목 안전 (S-6) | 계단실이 복도에서 직접 사선에 안 보임 | — | 계단실 입구가 20m+ 직선 복도에 직접 노출 | 전실(vestibule) 추가 |
| G-7 | 창문 배치 (S-7) | 외벽 방의 50%+ 에 창문 존재 | 30~50% | < 30% | 창문 추가 |
| G-8 | 은닉 공간 (S-8) | 유틸리티 Zone이 비직관적 위치에 ≥ 1개 | — | 유틸리티 Zone 0개 | 유틸리티 방 추가 |
| G-9 | 동선 다양성 | 임의 두 방 사이 경로 ≥ 2개 (BFS) | 일부 방 쌍에서 경로 1개 | 고립된 방 존재 | 연결 문/복도 추가 |
| G-10 | 고저차 활용 (S-10) | 메자닌/캣워크/반층 차이 ≥ 1개 | — | 2층+ 이나 고저차 요소 0 | 메자닌/캣워크 추가 |
| G-11 | 내부 랜드마크 (S-12) | 일반 방 크기의 3배+ 공간 ≥ 1개 | — | 모든 방 유사 크기 | 특수 대형 공간 추가 |
| G-12 | 엄폐 밀도 | 10m+ 개방 사선에 엄폐 후보 ≥ 1개/10m | — | 20m+ 사선에 엄폐 0 | 기둥/벽 돌출 추가 |

### 엄폐 후보 자동 마킹

실내 레이아웃 생성 후, 엄폐물 배치가 필요한 위치를 자동 마킹한다. 실제 엄폐물 메쉬 배치는 기획자 또는 아트팀이 수행.

| 마커 유형 | 배치 조건 | 아이콘 |
|----------|----------|-------|
| `cover_low` | 10m+ 직선 사선 중간 지점. 높이 1.0m | ▬ (낮은 엄폐) |
| `cover_high` | 15m+ 직선 사선 중간 지점. 높이 2.0m | ▮ (높은 엄폐) |
| `cover_pillar` | 대공간 20m+ 교차 사선 교점 | ● (기둥) |
| `cover_corner` | T자/L자 복도 교차점에서 2m 후퇴 | ◢ (코너) |

> 엄폐 후보 마커는 `structure_layout.json`에 `cover_hints` 배열로 저장. 실제 메쉬로 교체 전까지 뷰포트에 반투명 프록시로 표시.

### G-1~G-12 산출 알고리즘

검증 체크리스트 각 항목의 구체적 산출 방법.

**G-1 진입/탈출 경로**: `structure_layout.json`의 `entrances` 배열 순회. 각 출입구의 `facing_deg`로 면(N/E/S/W) 분류. 서로 다른 면 카운트.

**G-3 복도 리듬**: 각 복도의 `path_ue5`를 순회하며 문/분기점 간 거리를 측정.

```
ComputeCorridorRhythm(corridor):
  last_break = corridor.path_ue5[0]
  max_gap = 0
  for each door/branch on corridor:
    gap = Distance(last_break, door.location)
    max_gap = Max(max_gap, gap)
    last_break = door.location
  return max_gap
```

| 판정 | max_gap 값 |
|------|-----------|
| Pass | ≤ 15m |
| Warning | 15~20m |
| Fail | > 20m |

**G-5 교전 거리 다양성**: 모든 방의 대각선 길이를 산출하여 3개 구간으로 분류.

```
ClassifyEngagementRange(rooms):
  ranges = { CQB: 0, Mid: 0, Long: 0 }
  for each room:
    diag = sqrt(room.width² + room.depth²)
    if diag < 5m:    ranges.CQB++
    elif diag < 15m: ranges.Mid++
    else:            ranges.Long++
  total = sum(ranges)
  return { CQB: ranges.CQB/total, Mid: ranges.Mid/total, Long: ranges.Long/total }
```

| 판정 | 조건 |
|------|------|
| Pass | CQB 20~40% AND Mid 40~60% AND Long 10~30% |
| Warning | 1개 구간이 기준 외 |
| Fail | 1개 구간 0% |

**G-6 계단실 병목 안전**: 각 계단실 입구에서 복도 방향으로 Raycast(2D). 첫 벽/코너까지 거리를 측정.

```
CheckStairExposure(stair, corridors):
  for each stair entrance door:
    ray_origin = door.location
    ray_dir = corridor direction at door
    hit_dist = Raycast2D(ray_origin, ray_dir, walls + corners)
    if hit_dist > 20m: return Fail
  return Pass
```

**G-9 동선 다양성**: 방-문 관계를 그래프로 변환 후 BFS.

```
CheckPathDiversity(rooms, doors):
  graph = BuildAdjacencyGraph(rooms, doors)
  // 각 방이 노드, 문이 간선
  for each pair (room_a, room_b):
    paths = FindKShortestPaths(graph, room_a, room_b, K=2)
    if paths.count < 2:
      if paths.count == 0: return Fail  // 고립
      isolated_pairs.add(pair)
  if isolated_pairs.count > total_pairs * 0.2: return Warning
  return Pass
```

> `FindKShortestPaths`는 Yen의 K-최단 경로 알고리즘 사용. K=2로 제한하여 성능 유지 (방 수 < 100이므로 O(K·V·E) 허용).

**G-12 엄폐 밀도**: 각 방/복도 내부에서 4방향 Raycast(2D)로 벽까지 최장 사선 측정.

```
CheckCoverDensity(space):
  max_open = 0
  for each 2m grid point in space:
    for dir in [N, E, S, W]:
      dist = Raycast2D(point, dir, walls)
      if dist > max_open: max_open = dist
  // max_open 구간에 cover_hints가 있는지 확인
  for each sightline > 10m:
    covers_in_sightline = CountCoversAlongLine(...)
    if covers_in_sightline < ceil(sightline_length / 10): return Warning
  return Pass
```

---

## 내부 메커니즘

### 솔리드 → 모듈러 전환 절차

1단계에서 배치된 솔리드 메쉬(`AStaticMeshActor`)를 1.5단계에서 모듈러 화이트박스로 교체하는 절차.

| 단계 | 동작 | 상세 |
|------|------|------|
| 1 | 대상 건물 Actor 조회 | `stable_id`로 기존 `AStaticMeshActor` 검색 |
| 2 | 기존 Actor **숨김** | `SetActorHiddenInGame(true)` + `SetActorEnableCollision(false)`. **삭제하지 않음** (복원 가능성 유지) |
| 3 | `structure_generate` Edit Layer 생성 | 기존 건물의 `stable_id`를 `target_stable_id`로 참조 |
| 4 | 외관 모듈 조립 | Footprint 기반 외벽·지붕 모듈 스폰 |
| 5 | 실내 레이아웃 생성 | Zone → BSP → Corridor → Whitebox 조립 |
| 6 | 모듈 Actor에 `stable_id` 부여 | `struct_{원본stable_id}_{module_index}` 형식. 예: `struct_bldg_osm_123_wall_042` |
| 7 | `StructureActorMap`에 매핑 | `structure_id → TArray<AActor*>` (전체 모듈 목록) |
| 8 | `has_interior = true` 갱신 | `buildings.json` 메모리 캐시 + `map_meta.json` 갱신 |

**되돌리기 (구조물 삭제)**:

| 단계 | 동작 |
|------|------|
| 1 | `structure_generate` 레이어 삭제 (Edit Layer 표준 절차) |
| 2 | `StructureActorMap`에서 해당 구조물의 모듈 Actor 전체 삭제 |
| 3 | 원본 솔리드 Actor 복원: `SetActorHiddenInGame(false)` + `SetActorEnableCollision(true)` |
| 4 | `has_interior = false` 갱신 |

> 원본 Actor를 삭제하지 않고 숨기는 이유: 1) 구조물 생성 취소 시 즉시 복원 가능, 2) 2단계 슬라이더가 원본 건물 데이터를 참조할 때 일관성 유지, 3) Undo 1회로 전체 복원 가능 (1 Transaction)

**stable_id 매핑 체계**:

| Actor 유형 | stable_id 형식 | 예시 |
|-----------|---------------|------|
| 원본 솔리드 (숨김 상태) | `bldg_osm_{id}` (기존 유지) | `bldg_osm_1234567` |
| 외벽 모듈 | `struct_{원본id}_ext_{index}` | `struct_bldg_osm_1234567_ext_012` |
| 내벽 모듈 | `struct_{원본id}_int_{index}` | `struct_bldg_osm_1234567_int_045` |
| 바닥/천장 | `struct_{원본id}_floor_{층}_{index}` | `struct_bldg_osm_1234567_floor_1_003` |
| 계단 | `struct_{원본id}_stair_{id}` | `struct_bldg_osm_1234567_stair_east` |
| 캣워크/난간 | `struct_{원본id}_cat_{index}` | `struct_bldg_osm_1234567_cat_001` |

### 비직사각형 Footprint 처리

OSM footprint은 대부분 비정형 다각형(볼록/오목)이다. Squarified Treemap은 직사각형 입력을 요구하므로, 변환 절차가 필요하다.

**Footprint 정규화 전략**:

| 단계 | 동작 | 상세 |
|------|------|------|
| 1 | Footprint 폴리곤 분류 | 볼록(convex) / 오목(concave) 판정. 교차곱 부호 검사 |
| 2-a | **볼록 다각형** | OBB(Oriented Bounding Box) 산출 → 최소 면적 직사각형 |
| 2-b | **오목 다각형** | Convex Decomposition (Hertel-Mehlhorn) → 볼록 부분 폴리곤 분할 |
| 3 | 각 볼록 부분에 OBB 산출 | 부분별 독립 직사각형 |
| 4 | **단일 블록**: OBB를 Treemap 입력으로 사용 | 볼록 Footprint |
| 5 | **다중 블록**: 가장 큰 OBB를 주동(Main Wing), 나머지를 별관(Sub Wing)으로 지정 | L자, T자, ㄷ자 건물 |
| 6 | 주동에 Treemap 적용 (Zone 할당 주 영역) | 로비·주요 Zone 배치 |
| 7 | 별관에 보조 Zone 배치 | 유틸리티, 부속실, 계단실 등 |
| 8 | Wing 간 연결 복도 자동 생성 | 접합 Edge에서 복도 폭 확보 |

**Wing 분할 판정 기준**:

| 조건 | 처리 |
|------|------|
| OBB 면적 / Footprint 면적 > 0.85 | 단일 블록 (정규화 불필요) |
| OBB 면적 / Footprint 면적 ≤ 0.85 | 다중 Wing 분할 |
| Convex Decomposition 결과 2~4개 | 2~4 Wing |
| Convex Decomposition 결과 5개+ | 상위 3개만 Wing, 나머지는 작은 부분 병합 |

**Zone 면적 오차 보정**:

Treemap 후 각 Zone AABB가 실제 Footprint 폴리곤과 교차 검사. Footprint 외부에 걸치는 면적을 산출하고, Zone 실효 면적 = AABB 면적 - 외부 면적. 실효 면적이 목표의 70% 미만이면 인접 Zone과 경계 재조정.

### Edit Layer ↔ structure_layout.json 관계

구조물 데이터가 두 곳에 존재하는 이유와 우선순위.

**역할 분리**:

| 데이터 | 위치 | 역할 | 누가 읽는가 |
|--------|------|------|-----------|
| `structure_generate` 레이어 | `layers.json` | "이 건물에 구조물 생성이 적용되었다"는 **트리거 기록** | Edit Layer Stack (유효 상태 재구성) |
| 실제 레이아웃 상세 | `structure_layout.json` | Zone, Room, Corridor, 모듈 배치의 **상세 데이터** | Structure Generator Subsystem |
| `structure_room_modify` 등 | `layers.json` | 기획자 편집의 **변경 이력** (Undo/Redo용) | Edit Layer Stack |
| `edit_operations` 배열 | `structure_layout.json` | 동일 편집의 **적용 결과** (레이아웃에 이미 반영) | Structure Generator Subsystem |

**유효 상태 재구성 절차에서의 우선순위**:

```
RebuildEffectiveState():
  // 기존 건물/도로/지형 재구성 (기존 보완문서 v4.4 절차)
  ...
  // 구조물 재구성 (신규)
  for each layer where type == "structure_generate":
    structure_id = layer.params.target_stable_id
    layout = LoadStructureLayout(structure_id)
    if layout exists AND layout.version matches:
      // structure_layout.json이 이미 최신 → 그대로 사용
      RegisterStructureModules(layout)
    else:
      // layout 없거나 버전 불일치 → 재생성
      RegenerateStructure(layer.params)
```

**충돌 해소 규칙**:

| 상황 | 해소 |
|------|------|
| `layers.json`에 `structure_generate`가 있으나 `structure_layout.json` 누락 | 레이어 params의 seed + zone_ratios로 재생성. Warning 표시 |
| `structure_layout.json`은 있으나 `layers.json`에 레이어 없음 | Orphan 상태. "구조물 연결 끊김" 경고 + 레이어 자동 복원 옵션 |
| 두 파일의 편집 이력 불일치 | `layers.json`의 편집 레이어를 `structure_layout.json`에 재적용 (레이어가 Truth) |
| 2단계 `building_remove`가 구조물 대상 건물을 제거 | 구조물 전체 비활성화 (모듈 숨김) + Warning: "구조물이 포함된 건물이 제거됨" |
| 2단계 `building_height`가 구조물 대상 건물에 적용 | 무시. 구조물은 자체 층수/높이 관리 |

> **Truth 원칙**: `layers.json`이 "무엇이 적용되었는가"의 Truth. `structure_layout.json`은 "그 결과 레이아웃이 어떻게 생겼는가"의 캐시. 충돌 시 항상 `layers.json` 기준으로 재구성.

### Zone 비율 조정 시 연쇄 효과 규칙

기획자가 Zone A의 비율을 변경할 때, 나머지 Zone의 축소/확대 규칙.

**비율 재분배 알고리즘**:

```
AdjustZoneRatio(changed_zone, new_ratio):
  delta = new_ratio - changed_zone.current_ratio
  other_zones = all zones except changed_zone
  
  // 1단계: 조정 가능한 여유분 산출
  for each zone in other_zones:
    if delta > 0:  // 확대 → 다른 Zone 축소 필요
      zone.available = zone.current_ratio - zone.min_ratio
    else:          // 축소 → 다른 Zone 확대 가능
      zone.available = zone.max_ratio - zone.current_ratio
  
  total_available = sum(zone.available for zone in other_zones)
  
  // 2단계: delta가 total_available 이내인지 검증
  if abs(delta) > total_available:
    // 불가능 → 가능한 최대치로 클램핑
    new_ratio = changed_zone.current_ratio + sign(delta) * total_available
    delta = new_ratio - changed_zone.current_ratio
    ShowWarning("다른 Zone의 최소/최대 제약으로 {new_ratio}%까지만 조정 가능")
  
  // 3단계: 비례 분배
  for each zone in other_zones:
    share = zone.available / total_available
    zone.new_ratio = zone.current_ratio - delta * share
  
  // 4단계: 합계 100% 검증 + 부동소수점 보정
  Normalize(all zone ratios to sum 1.0)
```

**조정 순서 규칙**:

| 규칙 | 상세 |
|------|------|
| 복도/통로는 최후 축소 | 복도 비율이 `min_ratio` 이하로 떨어지면 동선 단절. 축소 우선순위 최하위 |
| 계단실은 축소 불가 | 층당 최소 2개 계단실 유지. 비율 고정 |
| 특수 대형 공간 축소 시 경고 | 체육관/생산라인 비율이 10% 미만이면 "대형 공간이 사라집니다" 확인 다이얼로그 |

### 에디터 세션 복원 절차

에디터 재시작 시, 구조물 모듈 Actor와 레이아웃 데이터의 일관성을 복원한다.

| 단계 | 동작 | 설명 |
|------|------|------|
| 1 | `layers.json` 로드 | `structure_generate` 레이어 목록 확보 |
| 2 | 각 구조물의 `structure_layout.json` 로드 | 레이아웃 데이터 복원 |
| 3 | **HISM Component 확인** | 레벨 저장 시 HISM이 직렬화되었으면 이미 존재 |
| 4-a | HISM **존재**: `StructureActorMap`에 매핑 등록. 스폰 스킵 | 중복 생성 방지 |
| 4-b | HISM **미존재** (레벨 미저장): `RebuildStructureModules()` 호출하여 재스폰 | 전체 모듈 재조립 |
| 5 | 원본 솔리드 Actor 상태 확인 | `structure_generate` 레이어가 active → 솔리드 숨김 유지 |
| 6 | `edit_operations` 재적용 | `structure_layout.json`에 이미 반영이므로 스킵. 불일치 시만 재적용 |

> 세션 복원은 `UStructureGeneratorSubsystem::Initialize()` 내에서 자동 수행된다. 복원 실패한 구조물은 `⚠ 구조물 복원 실패` 상태로 표시하고, 원본 솔리드 Actor를 자동 복원(숨김 해제).

### 에러 핸들링

| 상황 | 처리 |
|------|------|
| `structure_layout.json` 파싱 실패 | `OnLog(Error)` + 원본 솔리드 Actor 복원. 손상 파일 `.bak` 백업 |
| Footprint 꼭짓점 < 3개 | 구조물 생성 거부. "유효하지 않은 Footprint" 메시지 |
| Zone Treemap 실패 (면적 < min_area) | 가장 작은 Zone 제외 후 재시도. 2회 실패 시 단일 Zone(Open) 폴백 |
| BSP 분할 시 방 크기 min 미달 | 해당 리프 노드를 인접 노드와 병합 |
| 복도 연결 시 고립된 방 발생 | 가장 가까운 복도/방으로 강제 문 생성. Warning 로그 |
| 모듈 스폰 시 HISM 한도 초과 | 잔여 모듈을 개별 `AStaticMeshActor`로 폴백. Warning 로그 |
| 원본 솔리드 Actor 미존재 (삭제됨) | 구조물 레이어를 Orphan 상태로 표시. 모듈은 유지 (독립 구조물) |
| 화이트박스 메쉬 에셋 미존재 | `/Engine/BasicShapes/Cube` 폴백. Warning: "WB_Wall 메쉬 없음" |

### 성능 예산 — 다수 구조물 동시 존재

| 항목 | 예산 | 근거 |
|------|------|------|
| 맵당 구조물 최대 수 | 15개 | 대형 맵(반경 5km) 기준 POI 15개 상한 |
| 구조물당 모듈 수 | < 2,000개 | 1,000m² × 3층 기준 ~1,500 모듈 |
| 맵 전체 모듈 수 | < 30,000개 | 15 × 2,000 |
| HISM 인스턴스 그룹 | 모듈 종류 14 × 구조물 15 = 210 HISM Component | 드로콜 최소화 |
| HISM 인스턴스 총 수 | < 30,000 | UE5 HISM은 100K+ 인스턴스 지원 |
| 메모리 (모듈 데이터) | ~50MB | 30K 인스턴스 × 트랜스폼(48B) + 메쉬 참조 |
| 생성 시간 (단일 구조물) | < 3초 | BSP + 조립 |
| 생성 시간 (전체 15개) | < 30초 | 순차 생성. 백그라운드 + 프로그레스 바 |
| 진단 시간 (단일 구조물) | < 2초 | G-1~G-12 전 항목 |
| `structure_layout.json` 크기 | < 500KB/구조물 | 2,000 모듈 × ~200B JSON |

**맵 규모별 스케일링**:

| 맵 반경 | 권장 구조물 수 | 구조물당 모듈 상한 | 비고 |
|---------|-------------|----------------|------|
| 1km | 3~5개 | 2,000 | 소형 맵. 전체 < 10K 모듈 |
| 2km | 5~8개 | 2,000 | 중형 맵. 전체 < 16K 모듈 |
| 3km | 8~12개 | 1,500 | 대형 맵. 모듈 밀도 제한 권장 |
| 5km | 10~15개 | 1,500 | 초대형 맵. 원거리 구조물 LOD 적용 |

> 구조물 수가 권장치를 초과하면 Warning: "구조물 {N}개 — 성능 영향 가능. 일부 구조물을 솔리드로 되돌리는 것을 권장합니다."

---

## 기존 시스템 통합

### buildings.json 확장

1단계 `buildings.json`에 대형 구조물 관련 필드를 추가한다.

| 필드 | 타입 | 설명 | 예시 |
|------|------|------|------|
| `is_large_structure` | bool | 대형 구조물 판정 결과 | `true` |
| `structure_type` | string | 구조물 세부 유형 | `"factory"`, `"hospital"`, `"school"` |
| `floor_count` | int | 산출된 층수 | `3` |
| `floor_height_m` | float | 층고 | `3.5` |
| `has_interior` | bool | 실내 생성 완료 여부 | `false` (생성 전) |

> 기존 필드(`id`, `type`, `height_m`, `area_m2`, `centroid_ue5`, `footprint_ue5`)는 변경 없음.

### Edit Layer 확장

기존 Edit Layer 타입에 구조물 관련 타입을 추가한다.

| 신규 type | params | 설명 |
|-----------|--------|------|
| `structure_generate` | `{ "target_stable_id": "bldg_osm_xxx", "structure_type": "factory", "zone_ratios": {...}, "seed": 12345 }` | 구조물 실내 생성 트리거 |
| `structure_room_modify` | `{ "structure_id": "...", "room_id": "...", "operation": "resize\|remove\|split\|merge", "params": {...} }` | 방 편집 |
| `structure_wall_modify` | `{ "structure_id": "...", "wall_id": "...", "new_module": "WB_Wall_Door" }` | 벽/문/창문 변경 |
| `structure_stairs_add` | `{ "structure_id": "...", "location_ue5": [x,y,z], "type": "180_turn\|straight" }` | 계단 추가 |
| `structure_zone_modify` | `{ "structure_id": "...", "zone_ratios": {...} }` | Zone 비율 변경 |

> 구조물 Edit Layer는 `created_by: "structure_gen" | "structure_edit"` 으로 구분. 기존 `building_remove`, `building_height` 등은 구조물 외부에서의 조작에 사용.

### Building Pool 확장 — 화이트박스 모듈 등록

`ULevelToolBuildingPool`에 화이트박스 모듈을 등록하는 방식 2가지:

**방식 A: 기존 Pool 확장** — `FBuildingMeshEntry`에 `TypeKey = "WB_Wall"` 등으로 화이트박스 모듈 추가

**방식 B: 별도 Module Pool** (권장) — `UStructureModulePool` 신규 DataAsset 생성

```
UStructureModulePool (DataAsset)
├── FStructureModuleEntry
│   ├── ModuleId: "WB_Wall"
│   ├── Mesh: TSoftObjectPtr<UStaticMesh>
│   ├── Size: FVector (4, 0.2, 3.2)
│   ├── Category: EModuleCategory (Wall, Floor, Stairs, Railing, Roof)
│   ├── bSupportsHISM: true
│   └── CollisionProfile: "BlockAll"
└── FallbackModule: SM_Cube
```

> 방식 B를 권장하는 이유: 화이트박스 모듈은 건물 배치용 메쉬(`FBuildingMeshEntry`)와 목적·크기·사용 패턴이 다르므로 분리 관리가 유지보수에 유리.

### structure_layout.json

각 구조물의 실내 레이아웃 데이터. `layers.json`과 별도 파일.

저장 경로: `{맵폴더}/structures/{stable_id}_layout.json`

```json
{
  "version": "1.0",
  "structure_id": "bldg_osm_1234567",
  "structure_type": "school",
  "footprint_ue5": [[x,y], ...],
  "floor_count": 3,
  "floor_height_m": 3.5,
  "generation_seed": 42,
  "zone_ratios": {
    "classroom": 0.35,
    "gymnasium": 0.15,
    "corridor": 0.25,
    "admin": 0.10,
    "cafeteria": 0.10,
    "stairs": 0.05
  },
  "floors": [
    {
      "floor_index": 0,
      "elevation_m": 0.0,
      "zones": [
        {
          "zone_id": "zone_0_lobby",
          "zone_type": "lobby",
          "aabb_ue5": { "min": [x,y], "max": [x,y] },
          "rooms": [
            {
              "room_id": "room_0_lobby_main",
              "aabb_ue5": { "min": [x,y], "max": [x,y] },
              "height_m": 7.0,
              "is_void": true,
              "doors": [
                { "wall_id": "wall_xxx", "position": "north", "type": "double" }
              ]
            }
          ]
        }
      ],
      "corridors": [
        {
          "corridor_id": "corr_0_main",
          "path_ue5": [[x,y], [x,y], ...],
          "width_m": 3.0,
          "pattern": "double_loaded"
        }
      ],
      "stairs": [
        {
          "stair_id": "stair_0_east",
          "location_ue5": [x,y],
          "size_m": [4, 5],
          "type": "180_turn",
          "connects_to_floor": [0, 1]
        }
      ]
    }
  ],
  "entrances": [
    {
      "entrance_id": "ent_0_main",
      "location_ue5": [x,y,z],
      "facing_deg": 180,
      "type": "double_door",
      "connected_room": "room_0_lobby_main"
    }
  ],
  "cover_hints": [
    {
      "location_ue5": [x,y,z],
      "type": "cover_pillar",
      "sightline_length_m": 25.0
    }
  ],
  "edit_operations": [
    {
      "op_id": "op_001",
      "type": "room_resize",
      "target": "room_1_class_03",
      "params": { "new_aabb": { "min": [x,y], "max": [x,y] } },
      "timestamp": "2026-03-20T14:30:00Z"
    }
  ]
}
```

### Designer Intent 시스템 연동

| 연동 지점 | 동작 |
|----------|------|
| 2단계 밀도 슬라이더 | 구조물은 단일 건물로 취급. 제거/추가 대상에서 제외 가능 (내부가 생성된 건물은 제거 우선순위 최하위) |
| 2단계 파괴도 슬라이더 | 구조물 외벽에 파괴 상태 적용 가능 (내벽은 유지) |
| 3단계 진단 | 구조물 내부 면적을 "엄폐 영역"으로 산입. G-1~G-12 검증을 3단계 체크 확장 항목으로 추가 가능 |
| 맵 적합도 대시보드 | 구조물 내부 품질 점수를 별도 섹션으로 표시 |

---

## 데이터 포맷

### Structure Template JSON

구조물 유형별 생성 규칙을 정의하는 템플릿. 에디터에 내장.

저장 경로: `{Plugin}/Config/StructureTemplates/`

```json
{
  "template_id": "factory_default",
  "display_name": "공장 (기본)",
  "structure_type": "factory",
  "floor_count_range": [1, 3],
  "floor_height_m": 5.0,
  "min_area_m2": 800,
  "zone_definitions": [
    {
      "zone_type": "production",
      "default_ratio": 0.40,
      "min_ratio": 0.20,
      "max_ratio": 0.60,
      "room_size_min_m": [20, 30],
      "room_size_max_m": [40, 60],
      "bsp_enabled": false,
      "floor_preference": "any",
      "special": { "high_ceiling": true, "catwalk_enabled": true }
    },
    {
      "zone_type": "control_room",
      "default_ratio": 0.10,
      "min_ratio": 0.05,
      "max_ratio": 0.20,
      "room_size_min_m": [4, 6],
      "room_size_max_m": [6, 8],
      "bsp_enabled": true,
      "floor_preference": "upper",
      "special": { "mezzanine": true, "observation_window": true }
    },
    {
      "zone_type": "storage",
      "default_ratio": 0.25,
      "min_ratio": 0.10,
      "max_ratio": 0.40,
      "room_size_min_m": [10, 15],
      "room_size_max_m": [20, 30],
      "bsp_enabled": false,
      "floor_preference": "ground"
    },
    {
      "zone_type": "loading_dock",
      "default_ratio": 0.10,
      "min_ratio": 0.05,
      "max_ratio": 0.20,
      "room_size_min_m": [8, 12],
      "room_size_max_m": [12, 20],
      "bsp_enabled": false,
      "floor_preference": "ground",
      "special": { "exterior_facing": true, "shutter_door": true }
    },
    {
      "zone_type": "utility",
      "default_ratio": 0.10,
      "min_ratio": 0.05,
      "max_ratio": 0.15,
      "room_size_min_m": [3, 4],
      "room_size_max_m": [5, 6],
      "bsp_enabled": true,
      "floor_preference": "ground",
      "special": { "hidden_access": true }
    },
    {
      "zone_type": "corridor",
      "default_ratio": 0.05,
      "min_ratio": 0.03,
      "max_ratio": 0.10,
      "width_m": 3.0,
      "pattern_preference": "open"
    }
  ],
  "facade_rules": {
    "ground_floor": { "shutter": 0.30, "wall": 0.50, "window": 0.20 },
    "upper_floors": { "clerestory": 0.60, "wall": 0.40 }
  },
  "roof_type": "sawtooth",
  "entrance_rules": {
    "min_count": 2,
    "min_separation_ratio": 0.25,
    "prefer_road_facing": true,
    "types": ["door", "shutter"]
  },
  "stairs_rules": {
    "min_per_floor": 2,
    "max_spacing_m": 40,
    "prefer_ends": true
  },
  "gameplay_overrides": {
    "catwalk_height_m": 5.0,
    "mezzanine_ratio": 0.3
  }
}
```

> 6종 구조물 유형마다 1개의 기본 템플릿을 제공. 기획자가 커스텀 템플릿을 추가·수정 가능.

### Whitebox Module Registry

화이트박스 모듈 피스의 카탈로그.

```json
{
  "version": "1.0",
  "modules": [
    {
      "module_id": "WB_Wall",
      "category": "wall",
      "size_m": [4.0, 0.2, 3.2],
      "mesh_path": "/Game/LevelTool/Structures/Whitebox/SM_WB_Wall",
      "supports_hism": true,
      "variations": [],
      "collision": "BlockAll"
    },
    {
      "module_id": "WB_Wall_Window",
      "category": "wall",
      "size_m": [4.0, 0.2, 3.2],
      "mesh_path": "/Game/LevelTool/Structures/Whitebox/SM_WB_Wall_Window",
      "supports_hism": true,
      "opening": { "width_m": 1.2, "height_m": 1.5, "offset_from_floor_m": 0.9 }
    },
    {
      "module_id": "WB_Wall_Door",
      "category": "wall",
      "size_m": [4.0, 0.2, 3.2],
      "mesh_path": "/Game/LevelTool/Structures/Whitebox/SM_WB_Wall_Door",
      "supports_hism": true,
      "opening": { "width_m": 1.0, "height_m": 2.2, "offset_from_edge_m": 0.5 }
    },
    {
      "module_id": "WB_Wall_Shutter",
      "category": "wall",
      "size_m": [6.0, 0.2, 4.0],
      "mesh_path": "/Game/LevelTool/Structures/Whitebox/SM_WB_Wall_Shutter",
      "supports_hism": false,
      "opening": { "width_m": 5.0, "height_m": 3.5 }
    },
    {
      "module_id": "WB_Floor",
      "category": "floor",
      "size_m": [4.0, 4.0, 0.2],
      "mesh_path": "/Game/LevelTool/Structures/Whitebox/SM_WB_Floor",
      "supports_hism": true
    },
    {
      "module_id": "WB_Stairs",
      "category": "stairs",
      "size_m": [3.0, 4.0, 3.2],
      "mesh_path": "/Game/LevelTool/Structures/Whitebox/SM_WB_Stairs_180",
      "supports_hism": false
    },
    {
      "module_id": "WB_Stairs_Straight",
      "category": "stairs",
      "size_m": [1.5, 6.0, 3.2],
      "mesh_path": "/Game/LevelTool/Structures/Whitebox/SM_WB_Stairs_Straight",
      "supports_hism": false
    },
    {
      "module_id": "WB_Catwalk",
      "category": "catwalk",
      "size_m": [4.0, 1.5, 0.1],
      "mesh_path": "/Game/LevelTool/Structures/Whitebox/SM_WB_Catwalk",
      "supports_hism": true,
      "requires_railing": true
    },
    {
      "module_id": "WB_Railing",
      "category": "railing",
      "size_m": [4.0, 0.1, 1.1],
      "mesh_path": "/Game/LevelTool/Structures/Whitebox/SM_WB_Railing",
      "supports_hism": true
    },
    {
      "module_id": "WB_Pillar",
      "category": "structural",
      "size_m": [0.4, 0.4, 3.2],
      "mesh_path": "/Game/LevelTool/Structures/Whitebox/SM_WB_Pillar",
      "supports_hism": true
    },
    {
      "module_id": "WB_Corner",
      "category": "structural",
      "size_m": [0.2, 0.2, 3.2],
      "mesh_path": "/Game/LevelTool/Structures/Whitebox/SM_WB_Corner",
      "supports_hism": true
    },
    {
      "module_id": "WB_Roof_Flat",
      "category": "roof",
      "size_m": [4.0, 4.0, 0.2],
      "mesh_path": "/Game/LevelTool/Structures/Whitebox/SM_WB_Roof_Flat",
      "supports_hism": true
    },
    {
      "module_id": "WB_Roof_Slope",
      "category": "roof",
      "size_m": [4.0, 4.0, 2.0],
      "mesh_path": "/Game/LevelTool/Structures/Whitebox/SM_WB_Roof_Slope",
      "supports_hism": false
    }
  ]
}
```

---

## UI 와이어프레임

### Structure Editor 패널 (뷰포트 우측)

```
┌─────────────────────────────────────────────────┐
│ ◆ Structure Generator                     [✕]   │
├─────────────────────────────────────────────────┤
│                                                  │
│  대상: BP_Building_Industrial (1500m²)           │
│  유형: [공장 ▼]          층수: [2 ▼]             │
│  층고: [5.0m ▼]          시드: [42    ] [🔄]     │
│                                                  │
├─────────────────────────────────────────────────┤
│  ◈ Zone 비율                                     │
│                                                  │
│  생산라인  [■■■■■■■■□□] 40%  [◄][►]             │
│  창고      [■■■■■□□□□□] 25%  [◄][►]             │
│  제어실    [■■□□□□□□□□] 10%  [◄][►]             │
│  로딩독    [■■□□□□□□□□] 10%  [◄][►]             │
│  유틸리티  [■■□□□□□□□□] 10%  [◄][►]             │
│  복도/통로 [■□□□□□□□□□]  5%  [◄][►]             │
│                                합계: 100%        │
│                                                  │
│  [    생성    ]  [  재생성  ]  [  초기화  ]       │
│                                                  │
├─────────────────────────────────────────────────┤
│  ◈ 층별 평면도                   [1F][2F][3F]    │
│  ┌───────────────────────────────────────┐      │
│  │                                       │      │
│  │  ┌─────────┐ ┌───────────────────┐   │      │
│  │  │ 제어실   │ │                   │   │      │
│  │  │ (2F)    │ │   생산라인        │   │      │
│  │  ├─────────┤ │   (통층)          │   │      │
│  │  │ 유틸    │ │                   │   │      │
│  │  └────┤복├──┘ └──────┤복├────────┘   │      │
│  │  ┌────┤도├──────────┤도├─────────┐   │      │
│  │  │ 로딩독   │ │   창고            │   │      │
│  │  │ [셔터]   │ │                   │   │      │
│  │  └──────────┘ └───────────────────┘   │      │
│  │  [출입구]                   [출입구]   │      │
│  └───────────────────────────────────────┘      │
│                                                  │
│  범례: ─ 벽  ├복├ 복도  [셔터] 셔터도어          │
│                                                  │
├─────────────────────────────────────────────────┤
│  ◈ 게임플레이 검증                               │
│                                                  │
│  [검증 실행]                                     │
│                                                  │
│  ✅ G-1 진입/탈출 경로: Pass (출입구 3개)         │
│  ✅ G-2 반복+특수 혼합: Pass (Zone 5종)           │
│  ✅ G-3 복도 리듬: Pass                           │
│  ⚠️ G-5 교전 거리: Warning (장거리 35%)           │
│  ✅ G-9 동선 다양성: Pass (경로 2.4개 평균)       │
│  ⚠️ G-12 엄폐 밀도: Warning (25m 구간 1건)       │
│                                                  │
│  총점: 10/12 Pass, 2 Warning, 0 Fail             │
│                                                  │
└─────────────────────────────────────────────────┘
```

### 뷰포트 표시

| 요소 | 뷰포트 표시 |
|------|-----------|
| 외벽 | 흰색 불투명 화이트박스 |
| 내벽 | 연회색 반투명 (선택 시 불투명) |
| 바닥/천장 | 밝은 회색 |
| 문 개구부 | 파란색 윤곽선 |
| 창문 개구부 | 하늘색 윤곽선 |
| 계단 | 녹색 |
| 캣워크/난간 | 주황색 |
| 출입구 | 노란색 화살표 |
| 엄폐 후보 마커 | 빨간 반투명 프록시 |
| Zone 경계 | 점선 (호버 시 Zone 이름 표시) |
| 선택된 방 | 테두리 하이라이트 + 속성 패널 연동 |

### 뷰포트 컨텍스트 메뉴 (구조물 내부에서 우클릭)

```
┌──────────────────────┐
│ 방 선택              │
│ 방 크기 조정...      │
│ 방 분할              │
│ 방 병합              │
│ 방 삭제              │
│ ───────────────────  │
│ 문 추가              │
│ 문 제거              │
│ 창문 추가            │
│ 창문 제거            │
│ ───────────────────  │
│ 계단 추가            │
│ 캣워크 추가...       │
│ 기둥 추가            │
│ ───────────────────  │
│ 출입구 추가          │
│ 출입구 이동          │
│ ───────────────────  │
│ 층별 평면도 보기     │
│ 게임플레이 검증      │
└──────────────────────┘
```

---

## Phase 및 테스트 기준

### Phase 로드맵

| Phase | 내용 | 선행 조건 | 예상 규모 |
|-------|------|-----------|----------|
| **S-0** | 모듈러 화이트박스 메쉬 세트 제작 (14종) | 아트팀 협의. 콜리전·피벗·스케일 규약 확정 | 아트 작업 2~3일 |
| **S-A** | 외관 모듈러 조립 시스템 | S-0 완료 + 기존 Building Pool 구조 이해 | `UStructureModulePool`, Edge 분해, 파사드 규칙 |
| **S-B** | 실내 레이아웃 생성 엔진 | S-A 완료 | Zone Treemap, BSP, Corridor, Whitebox 조립 |
| **S-C** | 기획자 편집 UI + Edit Layer 통합 | S-B + Designer Intent Phase A 완료 | Structure Editor 패널, 평면도 뷰, 편집 도구 |
| **S-D** | 게임플레이 진단 (G-1~G-12) | S-C + Designer Intent Phase B 완료 | 검증 체크리스트, 엄폐 마커, 제안 카드 |

### Phase 전제조건 상세

| Phase | 전제 | 확인 방법 |
|-------|------|----------|
| **S-0** | 화이트박스 메쉬 14종 FBX 납품 | `/Game/LevelTool/Structures/Whitebox/` 에 에셋 존재 |
| **S-A** | `FBuildingMeshEntry` 코드 이해 | 기존 Building Pool 코드 리뷰 |
| **S-B** | S-A 외관 조립이 Footprint 기반으로 동작 | S-A 테스트 통과 |
| **S-C** | Designer Intent 2단계 Edit Layer 시스템 가동 | Phase A 완료 |
| **S-D** | S-C 편집 가능 상태 + 3단계 체크리스트 엔진 가동 | Phase B 완료 |

### 테스트 기준

#### Phase S-A 완료 기준 (외관 모듈 조립)

| # | 테스트 | 합격 조건 |
|---|--------|----------|
| SA-1 | Edge 분해 정확도 | 직사각형 footprint → 4 Edge 정확 분해. 비정형 5각형 → 5 Edge |
| SA-2 | 모듈 배치 간극 | 인접 모듈 간 간극 < 1cm (시각적 틈 없음) |
| SA-3 | 파사드 규칙 적용 | 공장 1층에 셔터 비율 25~35%. 병원 상층에 창문 65~75% |
| SA-4 | 층수 결정 | OSM levels=4 → 4층. height=15m + 학교(3.5m/층) → 4층 |
| SA-5 | 지붕 배치 | 공장=톱날, 학교=평지붕, 교회=첨탑 정확 선택 |
| SA-6 | 성능 | 2000m² 건물 외관 조립 < 1초 |

#### Phase S-B 완료 기준 (실내 생성)

| # | 테스트 | 합격 조건 |
|---|--------|----------|
| SB-1 | Zone 비율 정확도 | 목표 비율 대비 ±5% 이내 |
| SB-2 | BSP 분할 결과 | 모든 방이 min~max 크기 범위 내. 종횡비 상한 미초과 |
| SB-3 | 복도 연결 | 모든 방에 문 ≥ 1개. 고립된 방 0개 |
| SB-4 | 계단실 배치 | 2층+ 시 층당 계단실 ≥ 2개. 전층 동일 XY 위치 |
| SB-5 | Whitebox 조립 | 벽·바닥·천장 간 간극 < 1cm. 겹침 0건 |
| SB-6 | 출입구 | 외벽 출입구 ≥ 2개, 서로 다른 면 |
| SB-7 | 성능 | 1000m² 3층 생성 < 3초 |
| SB-8 | 직렬화 왕복 | 생성 → `structure_layout.json` 저장 → 재로드 → 동일 결과 |

#### Phase S-C 완료 기준 (편집 UI)

| # | 테스트 | 합격 조건 |
|---|--------|----------|
| SC-1 | 방 크기 조정 | 드래그로 ±2m 조정 → 인접 방 자동 축소. 최소 크기 미달 시 차단 |
| SC-2 | 방 삭제/추가 | 삭제 → 복도 흡수. 추가 → 복도 분할 |
| SC-3 | 문/창문 변경 | 클릭 → 모듈 교체. 변경 횟수 50회 반복 → 누수 없음 |
| SC-4 | 계단 추가/이동 | 추가 → 전층 동기화. 이동 → 전층 XY 갱신 |
| SC-5 | Undo/Redo | 모든 편집 작업 Ctrl+Z/Y 동작. 10회 연속 Undo → 원래 상태 복원 |
| SC-6 | 평면도 뷰 | 층 전환 < 100ms. 편집↔평면도 동기화 |

#### Phase S-D 완료 기준 (게임플레이 진단)

| # | 테스트 | 합격 조건 |
|---|--------|----------|
| SD-1 | G-1~G-12 전 항목 | 알려진 "통과 레이아웃"과 "실패 레이아웃"에서 정확도 80%+ |
| SD-2 | 엄폐 후보 마킹 | 20m+ 사선에 엄폐 마커 자동 배치 |
| SD-3 | 제안 카드 | Fail 항목에 대해 구체적 수정 제안 생성 |
| SD-4 | 진단 성능 | 2000m² 3층 구조물 전체 진단 < 2초 |

---

## 추가 사양

### 모듈러 건물 파괴 상태 표현

기존 보완문서(v4.4)의 `destruction_state` (intact/partial/destroyed)를 모듈러 건물에 적용하는 규칙.

| 파괴 상태 | 모듈 처리 | 시각적 결과 |
|----------|----------|-----------|
| `intact` | 모든 모듈 정상 표시 | 온전한 건물 |
| `partial` | 외벽 모듈의 20~40%를 `WB_Wall_Damaged`로 교체 + 지붕 모듈 30% 제거 | 부분 파괴. 창문 깨짐, 벽 균열, 지붕 구멍 |
| `destroyed` | 외벽 모듈 60%+ 제거. 내벽 50% 제거. 바닥/천장 30% 제거. 잔여 모듈에 `WB_Wall_Ruin` 교체 | 폐허. 내부 노출, 잔해 더미 |

**파괴 모듈 선택 알고리즘**:

| 단계 | 동작 |
|------|------|
| 1 | 파괴 비율에 따라 제거/교체할 모듈 수 산출 |
| 2 | 건물 외곽에서부터 파괴 (외벽 → 지붕 → 내벽 → 바닥 순서) |
| 3 | 연속된 영역으로 파괴 (랜덤이 아닌 Perlin Noise 기반 클러스터링) |
| 4 | 구조적 안정성: 하층 벽이 제거되면 해당 상층 바닥/벽도 자동 제거 |
| 5 | 계단실은 최소 1개 유지 (파괴 상태에서도 수직 이동 가능) |

**추가 화이트박스 모듈 (파괴용)**:

| 모듈 ID | 설명 |
|---------|------|
| `WB_Wall_Damaged` | 균열 + 부분 개구부가 있는 벽 |
| `WB_Wall_Ruin` | 높이 50% 잔벽 |
| `WB_Rubble` | 잔해 더미 (바닥 배치) |
| `WB_Floor_Hole` | 구멍 뚫린 바닥 (수직 사선 개방) |

> 2단계 파괴도 슬라이더는 구조물 **외벽**에만 적용 가능. 내벽 파괴는 기획자가 수동으로 "방 벽 제거"를 통해 구현. 파괴 상태가 `destroyed`인 건물은 1.5단계 적용 전에 확인 다이얼로그 표시: "파괴된 건물의 실내를 생성하시겠습니까?"

### 커스텀 템플릿 저장/공유

기획자가 Zone 비율, 파사드 규칙 등을 커스텀 템플릿으로 저장하고 팀과 공유하는 체계.

**저장 포맷**: Structure Template JSON (섹션 9 참조)과 동일 스키마.

**저장 경로**: `{프로젝트}/Config/StructureTemplates/Custom/{template_id}.json`

**검증 절차** (로드 시 4단계):

| 단계 | 검증 | 실패 시 |
|------|------|--------|
| 1 | JSON 파싱 유효성 | 로드 거부 + Error 로그 |
| 2 | 스키마 버전 호환 (`version` 필드) | 마이그레이션 시도 → 실패 시 거부 |
| 3 | Zone 비율 합계 = 100% (±1% 허용) | 자동 정규화 + Warning |
| 4 | Zone별 min/max 범위 유효성 | min > max인 Zone 경고 + 기본값 폴백 |

**팀 공유 워크플로우**:

| 방법 | 절차 |
|------|------|
| VCS (Git) | `Config/StructureTemplates/Custom/` 폴더를 커밋. 다른 팀원은 pull 시 자동 인식 |
| 내보내기/가져오기 | Structure Editor → "템플릿 내보내기" → `.struct_template.json` 파일 저장. "템플릿 가져오기"로 로드 |
| 프로젝트 기본 | `Config/StructureTemplates/Default/`에 저장하면 모든 팀원이 기본 목록에서 확인 |

### 복합 건물 (다동 구조) 처리

학교(본관+별관), 병원(A동+B동) 등 복수의 동(Wing)으로 구성된 건물의 처리.

**OSM 데이터 입력 패턴**:

| 패턴 | 처리 |
|------|------|
| 단일 `way` + 복잡한 Footprint (L/T/ㄷ자) | 비직사각형 Footprint 처리 절차 적용 (Wing 자동 분할) |
| 별도 `way` 2~4개 + 동일 `relation` | `relation`의 member way를 **복합 구조물 1개**로 묶음 |
| 별도 `way` + relation 없음 | 독립 건물 취급. 기획자가 수동으로 "구조물 연결" 가능 |
| 별도 `way` + 동일 `name` 태그 | 이름 기반 자동 그루핑 제안 (확인 다이얼로그) |

**복합 구조물 데이터**:

```json
{
  "compound_structure_id": "compound_school_001",
  "wings": [
    {
      "wing_id": "main",
      "stable_id": "bldg_osm_111",
      "role": "primary",
      "structure_type": "school"
    },
    {
      "wing_id": "annex",
      "stable_id": "bldg_osm_222",
      "role": "secondary",
      "structure_type": "school"
    }
  ],
  "connections": [
    {
      "from_wing": "main",
      "to_wing": "annex",
      "floor": 1,
      "type": "corridor",
      "width_m": 3.0
    }
  ]
}
```

**Wing 간 연결 규칙**:

| 조건 | 자동 연결 | 연결 유형 |
|------|----------|----------|
| Wing 간 거리 < 5m | 자동 제안 | 밀폐 복도 |
| Wing 간 거리 5~15m | 자동 제안 | 외부 노출 복도 또는 2층 브릿지 |
| Wing 간 거리 > 15m | 연결 제안 안 함 | 기획자 수동 |
| Wing 간 층수 차이 ≥ 2 | 연결 층 지정 필요 | 낮은 Wing의 최상층에 연결 |

### 화이트박스 → 최종 메쉬 전환 경로

화이트박스 프로토타입을 최종 비주얼 메쉬로 교체하는 아트팀 협업 워크플로우.

**전환 단계**:

| 단계 | 담당 | 동작 |
|------|------|------|
| 1 | 기획자 | Structure Editor에서 레이아웃 확정. "아트 핸드오프" 버튼 클릭 |
| 2 | 시스템 | 레이아웃 데이터 내보내기: `{structure_id}_handoff.json` (방 목록, 크기, 문/창문 위치) + `.fbx` 참조용 메쉬 |
| 3 | 아트팀 | 핸드오프 데이터 기반으로 최종 메쉬 제작 (DCC 도구). 모듈별 또는 전체 단일 메쉬 |
| 4 | 아트팀 | 최종 메쉬를 `UStructureModulePool`에 등록 또는 별도 에셋으로 임포트 |
| 5 | 시스템 | "메쉬 교체" 기능: 화이트박스 모듈 → 최종 메쉬로 일괄 교체 |

**메쉬 교체 모드**:

| 모드 | 설명 | 장점 |
|------|------|------|
| **모듈 교체** | WB_Wall → SM_Final_Wall 등 1:1 교체 | 레이아웃 유지. 부분 교체 가능 |
| **전체 교체** | 모듈 전체 삭제 → 단일 최종 메쉬로 대체 | 최고 품질. 레이아웃 변경 불가 |
| **하이브리드** | 외벽만 최종 메쉬, 내벽은 화이트박스 유지 | 외관 품질 + 내부 편집 유연성 |

> 전환 경로는 Phase S-D 이후 별도 Phase (S-E)로 계획. 기획문서 범위에서는 "핸드오프 포맷 정의"까지만 확정.

### C++ API 초안 — `UStructureGeneratorSubsystem`

1.5단계 구조물 생성의 핵심 Subsystem 인터페이스.

```
UCLASS()
class UStructureGeneratorSubsystem : public UWorldSubsystem
{
  GENERATED_BODY()

public:
  // ── 생성 ──
  // 대상 건물에 구조물 실내 생성. 결과를 structure_layout.json에 저장.
  FStructureResult GenerateStructure(
    const FString& TargetStableId,
    const FStructureTemplate& Template,
    int32 Seed = 0);

  // 기존 구조물 재생성 (편집 초기화)
  FStructureResult RegenerateStructure(
    const FString& StructureId,
    const FStructureTemplate& Template,
    int32 Seed = 0);

  // 구조물 삭제 (원본 솔리드 복원)
  void RemoveStructure(const FString& StructureId);

  // ── 편집 ──
  // 방 크기 조정
  bool ResizeRoom(const FString& StructureId, const FString& RoomId,
    const FBox2D& NewAABB);

  // 방 추가/삭제/분할/병합
  FString AddRoom(const FString& StructureId, const FVector2D& Location,
    const FVector2D& Size);
  bool RemoveRoom(const FString& StructureId, const FString& RoomId);
  TArray<FString> SplitRoom(const FString& StructureId, const FString& RoomId,
    const FVector2D& SplitStart, const FVector2D& SplitEnd);
  FString MergeRooms(const FString& StructureId,
    const TArray<FString>& RoomIds);

  // 벽 모듈 변경
  bool ModifyWall(const FString& StructureId, const FString& WallId,
    const FString& NewModuleId);

  // 계단 추가/이동
  FString AddStairs(const FString& StructureId, const FVector& Location,
    EStairType Type);
  bool MoveStairs(const FString& StructureId, const FString& StairId,
    const FVector2D& NewLocation);

  // Zone 비율 변경
  bool ModifyZoneRatios(const FString& StructureId,
    const TMap<FString, float>& NewRatios);

  // ── 진단 ──
  // 게임플레이 검증 실행
  FStructureDiagnosticResult RunDiagnostics(const FString& StructureId);

  // 엄폐 후보 마킹
  TArray<FCoverHint> GenerateCoverHints(const FString& StructureId);

  // ── 조회 ──
  // 레이아웃 데이터 조회
  const FStructureLayout* GetLayout(const FString& StructureId) const;

  // 구조물 목록
  TArray<FString> GetAllStructureIds() const;

  // 대형 구조물 판정
  bool IsLargeStructure(const FString& StableId) const;

  // ── 세션 ──
  // 초기화 (에디터 시작/레벨 로드 시)
  virtual void Initialize(FSubsystemCollectionBase& Collection) override;

  // 직렬화
  bool SaveLayout(const FString& StructureId);
  bool LoadLayout(const FString& StructureId);

  // ── 델리게이트 ──
  DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
    FOnStructureGenerated, const FString&, StructureId,
    const FStructureResult&, Result);
  UPROPERTY(BlueprintAssignable)
  FOnStructureGenerated OnStructureGenerated;

  DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
    FOnStructureModified, const FString&, StructureId,
    const FString&, OperationType);
  UPROPERTY(BlueprintAssignable)
  FOnStructureModified OnStructureModified;

  DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
    FOnStructureRemoved, const FString&, StructureId);
  UPROPERTY(BlueprintAssignable)
  FOnStructureRemoved OnStructureRemoved;

private:
  // 구조물 ID → 레이아웃 데이터
  TMap<FString, FStructureLayout> LayoutCache;

  // 구조물 ID → 스폰된 모듈 Actor 목록
  TMap<FString, TArray<TWeakObjectPtr<AActor>>> StructureActorMap;

  // 모듈 Pool
  UPROPERTY()
  UStructureModulePool* ModulePool;
};
```

**핵심 Enum**:

```
UENUM(BlueprintType)
enum class EStairType : uint8
{
  Turn180    UMETA(DisplayName = "180° Turn"),
  Straight   UMETA(DisplayName = "Straight"),
  Spiral     UMETA(DisplayName = "Spiral")
};

UENUM(BlueprintType)
enum class ECorridorPattern : uint8
{
  SingleLoaded  UMETA(DisplayName = "Single-Loaded"),
  DoubleLoaded  UMETA(DisplayName = "Double-Loaded"),
  Ring          UMETA(DisplayName = "Ring"),
  Open          UMETA(DisplayName = "Open/Free")
};

UENUM(BlueprintType)
enum class EModuleCategory : uint8
{
  Wall, Floor, Ceiling, Stairs, Railing, Catwalk, Pillar, Corner, Roof
};

UENUM(BlueprintType)
enum class EDiagnosticSeverity : uint8
{
  Pass, Warning, Fail
};
```

**핵심 구조체**:

```
USTRUCT(BlueprintType)
struct FStructureResult
{
  GENERATED_BODY()
  bool bSuccess;
  FString StructureId;
  int32 ModuleCount;
  float GenerationTimeMs;
  TArray<FString> Warnings;
};

USTRUCT(BlueprintType)
struct FStructureDiagnosticResult
{
  GENERATED_BODY()
  TArray<FDiagnosticItem> Items;  // G-1 ~ G-12
  int32 PassCount;
  int32 WarningCount;
  int32 FailCount;
};

USTRUCT(BlueprintType)
struct FDiagnosticItem
{
  GENERATED_BODY()
  FString CheckId;        // "G-1", "G-2", ...
  FString Description;
  EDiagnosticSeverity Severity;
  FString Detail;         // "출입구 3개, 서로 다른 면"
  FVector FocusLocation;  // 뷰포트 카메라 이동용
  FString Suggestion;     // Fail 시 제안
};
```

---

## 기획 확정 사항

| # | 항목 | 확정 방향 | 버전 |
|---|------|-----------|------|
| 1 | 도구 성격 | 혼합 방식 — 기본 골격 자동 생성 + 기획자 세부 편집 | v1.0 |
| 2 | 실내 디테일 수준 | 게임플레이 프로토타입 (BSP/화이트박스). 최종 비주얼은 별도 | v1.0 |
| 3 | 파이프라인 위치 | 1.5단계 (선택적). 1단계 후 대형 건물에만 적용 | v1.0 |
| 4 | 대형 구조물 판정 | area ≥ 500m² OR height ≥ 15m OR 특정 TypeKey | v1.0 |
| 5 | 구조물 유형 | 6종: 공장, 창고, 병원, 학교, 상업, 교회 | v1.0 |
| 6 | Zone 할당 알고리즘 | Squarified Treemap | v1.0 |
| 7 | Room 분할 알고리즘 | BSP (Binary Space Partitioning) | v1.0 |
| 8 | 복도 패턴 | 4종: 편복도, 중복도, 루프, 자유형 | v1.0 |
| 9 | 화이트박스 모듈 | 14종 기본 + 4종 파괴용 = 18종 | v1.1 |
| 10 | 모듈 Pool | 별도 `UStructureModulePool` DataAsset (기존 Building Pool과 분리) | v1.0 |
| 11 | 최소 출입구 | 2개, 서로 다른 면 | v1.0 |
| 12 | 최소 계단실 | 층당 2개, 최대 간격 40m | v1.0 |
| 13 | 게임플레이 검증 | 12항 체크리스트 (G-1~G-12) + 산출 알고리즘 | v1.1 |
| 14 | 엄폐 후보 마킹 | 자동 마킹 (cover_hints). 실제 배치는 수동 | v1.0 |
| 15 | 비파괴 편집 | `structure_layout.json` + `edit_operations` 배열 | v1.0 |
| 16 | HISM 활용 | 동일 모듈은 HierarchicalInstancedStaticMesh로 인스턴싱 | v1.0 |
| 17 | 설계 원칙 | 12항 (S-1~S-12), 레퍼런스 7종에서 도출 | v1.0 |
| 18 | 인접 구조물 연결 | 옵션 제공 (발코니/통로), 기본은 독립 | v1.0 |
| 19 | Designer Intent 연동 | 2단계 슬라이더에서 구조물은 단일 POI 취급 | v1.0 |
| 20 | Phase 구조 | S-0→S-A→S-B→S-C→S-D 순차 | v1.0 |
| 21 | 솔리드→모듈러 전환 | 원본 Actor 숨김(삭제 아님) → 모듈 스폰 → 되돌리기 시 복원 | v1.1 |
| 22 | 비직사각형 Footprint | OBB + Convex Decomposition → Wing 분할. 면적비 0.85 기준 | v1.1 |
| 23 | 데이터 Truth 원칙 | `layers.json` = 트리거 Truth, `structure_layout.json` = 레이아웃 캐시 | v1.1 |
| 24 | Zone 비율 재분배 | 비례 분배 + min/max 클램핑 + 복도/계단 축소 제한 | v1.1 |
| 25 | 세션 복원 | HISM 존재 시 매핑만, 미존재 시 재스폰. 실패 시 솔리드 복원 | v1.1 |
| 26 | 에러 핸들링 | 8케이스 정의 (파싱 실패~메쉬 미존재). 폴백 전략 포함 | v1.1 |
| 27 | 성능 예산 | 맵당 < 15 구조물, 전체 < 30K 모듈, 생성 < 3초/건 | v1.1 |
| 28 | 모듈러 파괴 상태 | Perlin Noise 클러스터링 + 구조적 연쇄 + 계단 1개 유지 | v1.1 |
| 29 | 커스텀 템플릿 | JSON 저장 + 4단계 검증 + VCS/내보내기/가져오기 공유 | v1.1 |
| 30 | 복합 건물 (다동) | relation/name 기반 그루핑 + Wing 연결 (거리별 자동 제안) | v1.1 |
| 31 | 아트 핸드오프 | 핸드오프 JSON + 3가지 메쉬 교체 모드 (모듈/전체/하이브리드) | v1.1 |
| 32 | `UStructureGeneratorSubsystem` | C++ Subsystem API 초안 — 생성/편집/진단/직렬화 + 델리게이트 | v1.1 |
| 33 | 검증 산출 알고리즘 | BFS 동선, Raycast 사선, Yen K-최단경로 등 6개 알고리즘 | v1.1 |

---

## 변경 내역

| 버전 | 날짜 | 내용 |
|------|------|------|
| v1.0 | 2026-03-20 | 초안 작성. 11개 섹션 + 기획 확정 사항 20항 |
| v1.1 | 2026-03-20 | 구현 검증 보완 — P0 3건(솔리드→모듈러 전환, 비정형 Footprint, 데이터 Truth), P1 4건(세션 복원, 산출 알고리즘, Zone 연쇄, 성능 예산), P2 5건(파괴 상태, 템플릿 공유, 복합 건물, 메쉬 전환, C++ API). 기획 확정 사항 20→33항 |

---

— 기획문서 끝 (v1.1) —
