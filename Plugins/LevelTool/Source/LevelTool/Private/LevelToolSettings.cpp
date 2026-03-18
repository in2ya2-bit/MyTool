#include "LevelToolSettings.h"

ULevelToolSettings::ULevelToolSettings()
{
    CoordPresets.Add({"Seoul_Jongno",     37.5704f,  126.9820f, 1.0f});
    CoordPresets.Add({"Chernobyl",        51.3890f,   30.0993f, 1.5f});
    CoordPresets.Add({"Detroit_Downtown", 42.3314f,  -83.0457f, 1.2f});
    CoordPresets.Add({"Pripyat",          51.4072f,   30.0566f, 1.0f});
    CoordPresets.Add({"Incheon_Port",     37.4536f,  126.7020f, 1.0f});
}
