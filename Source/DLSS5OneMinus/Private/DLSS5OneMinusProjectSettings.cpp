#include "DLSS5OneMinusProjectSettings.h"

#include "DLSS5OneMinusProfile.h"
#include "Engine/World.h"

UDLSS5OneMinusProjectSettings::UDLSS5OneMinusProjectSettings()
{
    CategoryName = TEXT("Plugins");
    SectionName = TEXT("DLSS5-OneMinus");
}

FDLSS5OneMinusSettings UDLSS5OneMinusProjectSettings::ResolveDefaultSettings() const
{
    if (!DefaultProfile.IsNull())
    {
        if (const UDLSS5OneMinusProfile* Profile = DefaultProfile.LoadSynchronous())
        {
            return Profile->Settings.Normalized();
        }
    }
    return DefaultSettings.Normalized();
}

bool UDLSS5OneMinusProjectSettings::ShouldEnableForWorld(const UWorld* World) const
{
    if (World == nullptr)
    {
        return false;
    }
    return World->WorldType == EWorldType::Editor
        ? bEnableInEditorWorlds
        : bEnableInGameWorlds;
}
