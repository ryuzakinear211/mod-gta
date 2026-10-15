#ifndef FEATURE_UNLOCK_WEAPON_H
#define FEATURE_UNLOCK_WEAPON_H

#include <string>

namespace UnlockWeapon {

    // Initializes hooks for weapon unlock in lobby and in-game weapon override
    void Init();

    // Toggle unlock weapon feature on / off
    void SetEnabled(bool enable);
    bool IsEnabled();

    // Weapon selection details
    int GetSelectedWeaponId();
    std::string GetSelectedWeaponName();
    void SetSelectedWeapon(int weaponId, const std::string &name);

    // Formatted info for UI overlay
    std::string GetOverlayStatus();

} // namespace UnlockWeapon

#endif // FEATURE_UNLOCK_WEAPON_H
