#pragma once

#include "CoreMinimal.h"
#include "DesignerIntentTypes.h"
#include "PresetTypes.generated.h"

/**
 * 추천 좌표
 */
USTRUCT()
struct FPresetCoords
{
	GENERATED_BODY()

	float Lat       = 0.f;
	float Lon       = 0.f;
	float RadiusKm  = 2.f;
	FString Description;
};

/**
 * 슬라이더 5값 세트
 */
USTRUCT()
struct FSliderValues
{
	GENERATED_BODY()

	float UrbanDensity     = 50.f;
	float Openness         = 50.f;
	float RouteComplexity  = 50.f;
	float ElevationContrast= 50.f;
	float DestructionLevel = 0.f;

	float Get(ESliderType T) const
	{
		switch (T)
		{
		case ESliderType::UrbanDensity:      return UrbanDensity;
		case ESliderType::Openness:          return Openness;
		case ESliderType::RouteComplexity:   return RouteComplexity;
		case ESliderType::ElevationContrast: return ElevationContrast;
		case ESliderType::DestructionLevel:  return DestructionLevel;
		default: return 50.f;
		}
	}
};

/**
 * 맵 프리셋 (레퍼런스 / 커스텀 공용)
 */
USTRUCT()
struct FMapPreset
{
	GENERATED_BODY()

	FString        PresetName;
	FString        ReferenceGame;
	FString        ReferenceMap;
	FPresetCoords  RecommendedCoords;
	FSliderValues  Sliders;
	ERulesetType   Ruleset      = ERulesetType::BR;
	bool           bIsReference = true;

	// 커스텀 전용 필드
	FString        Author;
	FString        CreatedAt;
	FString        BaseReference;
	FString        Description;
	TArray<FString> ObjectPresetsApplied;
};

/**
 * 내장 레퍼런스 프리셋 11종 (BR 5 + EX 6)
 * 기획서 v4.4 명세에서 도출
 */
namespace LevelToolPresets
{

inline TArray<FMapPreset> GetBuiltInPresets()
{
	TArray<FMapPreset> P;
	P.Reserve(11);

	auto Make = [](const FString& Name, const FString& Game, const FString& Map,
		float Lat, float Lon, float R, const FString& Desc,
		float D, float O, float RC, float EC, float DL, ERulesetType RS) -> FMapPreset
	{
		FMapPreset M;
		M.PresetName    = Name;
		M.ReferenceGame = Game;
		M.ReferenceMap  = Map;
		M.RecommendedCoords = { Lat, Lon, R, Desc };
		M.Sliders = { D, O, RC, EC, DL };
		M.Ruleset       = RS;
		M.bIsReference  = true;
		return M;
	};

	// ── BR 5종 ──────────────────────────────────────────────────────
	P.Add(Make(TEXT("Erangel 스타일"),  TEXT("PUBG"), TEXT("Erangel"),
		59.95f, 30.32f, 3.f, TEXT("상트페테르부르크 교외 — 섬+해안+도시"),
		40, 60, 50, 40, 10, ERulesetType::BR));

	P.Add(Make(TEXT("Miramar 스타일"),  TEXT("PUBG"), TEXT("Miramar"),
		31.69f, -106.42f, 3.f, TEXT("엘패소 인근 사막 — 능선+산발 도시"),
		20, 80, 60, 70, 15, ERulesetType::BR));

	P.Add(Make(TEXT("Sanhok 스타일"),   TEXT("PUBG"), TEXT("Sanhok"),
		7.88f, 98.38f, 1.5f, TEXT("태국 푸켓 — 섬+밀집 건물"),
		70, 30, 70, 50, 5, ERulesetType::BR));

	P.Add(Make(TEXT("Vikendi 스타일"),  TEXT("PUBG"), TEXT("Vikendi"),
		42.65f, 18.09f, 2.f, TEXT("크로아티아 해안 — 산악+해안 마을"),
		35, 65, 45, 60, 10, ERulesetType::BR));

	P.Add(Make(TEXT("Verdansk 스타일"), TEXT("CoD Warzone"), TEXT("Verdansk"),
		50.45f, 30.52f, 2.5f, TEXT("키이우 교외 — 동유럽 도시+고층+산업"),
		75, 25, 65, 50, 20, ERulesetType::BR));

	// ── EX 6종 ──────────────────────────────────────────────────────
	P.Add(Make(TEXT("Dam 스타일"),      TEXT("ARC Raiders"), TEXT("Dam"),
		46.53f, 11.95f, 1.f, TEXT("이탈리아 알프스 댐 인근 — 산업+고저차"),
		60, 40, 50, 80, 30, ERulesetType::Extraction));

	P.Add(Make(TEXT("Spaceport 스타일"),TEXT("ARC Raiders"), TEXT("Spaceport"),
		28.57f, -80.65f, 2.f, TEXT("케이프 커내버럴 — 발사장+산업시설"),
		50, 50, 40, 30, 40, ERulesetType::Extraction));

	P.Add(Make(TEXT("Buried City 스타일"),TEXT("ARC Raiders"), TEXT("Buried City"),
		30.03f, 31.23f, 1.5f, TEXT("카이로 구도심 — 밀집 도시+사막"),
		80, 20, 60, 40, 50, ERulesetType::Extraction));

	P.Add(Make(TEXT("Customs 스타일"),  TEXT("Tarkov"), TEXT("Customs"),
		56.32f, 43.98f, 1.5f, TEXT("니즈니노브고로드 산업지대 — 선형 공장+주거"),
		45, 55, 35, 30, 25, ERulesetType::Extraction));

	P.Add(Make(TEXT("Shoreline 스타일"),TEXT("Tarkov"), TEXT("Shoreline"),
		43.36f, 39.96f, 2.f, TEXT("소치 해안 리조트 — 중앙 리조트+외곽 마을"),
		30, 70, 55, 45, 15, ERulesetType::Extraction));

	P.Add(Make(TEXT("Bayou 스타일"),    TEXT("Hunt: Showdown"), TEXT("Bayou"),
		29.95f, -90.07f, 1.f, TEXT("루이지애나 늪지 — 분산 농가+수변"),
		25, 75, 60, 15, 10, ERulesetType::Extraction));

	return P;
}

} // namespace LevelToolPresets
